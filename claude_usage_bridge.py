#!/usr/bin/env python3
"""
claude_usage_bridge.py — ponte entre o uso do plano Claude (Pro/Max) e o ESP32.

Roda no SEU computador (onde o Claude Code esta logado). Le o token OAuth que
o Claude Code mantem atualizado, consulta o endpoint de uso da Anthropic
(GET https://api.anthropic.com/api/oauth/usage) e expoe um JSON simplificado
em  http://<ip-do-pc>:8787/usage  para o Heltec WiFi Kit 32 (V3) buscar.

O token NUNCA sai do seu computador — o ESP32 so conversa com esta ponte.

Requisitos: Python 3.8+ (somente biblioteca padrao) e Claude Code logado.
Uso:        python3 claude_usage_bridge.py [porta]
"""

import json
import os
import platform
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
ANTHROPIC_URL = "https://api.anthropic.com/api/oauth/usage"
POLL_MIN_S = 60        # nao consulta a Anthropic mais que 1x por minuto
BACKOFF_429_S = 300    # se levar HTTP 429, espera 5 min antes de tentar de novo

_cache = {"data": None, "ts": 0.0, "wait_until": 0.0}
_lock = threading.Lock()


# ---------------------------------------------------------------- credenciais
def read_access_token() -> str:
    """Pega o accessToken OAuth que o Claude Code guarda localmente."""
    # macOS: Keychain
    if platform.system() == "Darwin":
        try:
            r = subprocess.run(
                ["security", "find-generic-password",
                 "-s", "Claude Code-credentials", "-w"],
                capture_output=True, text=True, timeout=10,
            )
            if r.returncode == 0 and r.stdout.strip():
                return json.loads(r.stdout.strip())["claudeAiOauth"]["accessToken"]
        except Exception:
            pass
    # Linux / Windows (e fallback no macOS): arquivo de credenciais
    for cand in ("~/.claude/.credentials.json",
                 "~/.config/claude/.credentials.json"):
        path = os.path.expanduser(cand)
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)["claudeAiOauth"]["accessToken"]
    raise RuntimeError(
        "Credenciais do Claude Code nao encontradas. "
        "Instale/abra o Claude Code e faca login com sua conta Pro/Max."
    )


# ------------------------------------------------------------------ utilidades
def _parse_iso(s):
    if not s:
        return None
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except ValueError:
        return None


def _fmt_left(seconds):
    """Formata tempo restante: '3h00', '1d15h', '45m'."""
    if seconds is None or seconds < 0:
        return "--"
    total_min = int(seconds // 60)
    d, rem = divmod(total_min, 1440)
    h, m = divmod(rem, 60)
    if d > 0:
        return f"{d}d{h:02d}h"
    if h > 0:
        return f"{h}h{m:02d}"
    return f"{m}m"


def _window(period, window_seconds):
    """Extrai % usado, tempo restante e 'ritmo' (uso% - tempo decorrido%).

    Ritmo negativo = voce esta gastando abaixo do ritmo da janela (bom).
    E o mesmo calculo dos sliders 'Session -33% / Weekly -62%' dos apps de menu.
    """
    out = {"pct": None, "left": "--", "pace": None}
    if not isinstance(period, dict) or period.get("utilization") is None:
        return out
    out["pct"] = round(float(period["utilization"]), 1)
    reset = _parse_iso(period.get("resets_at"))
    if reset:
        remaining = (reset - datetime.now(timezone.utc)).total_seconds()
        out["left"] = _fmt_left(remaining)
        if 0 <= remaining <= window_seconds:
            elapsed_pct = (window_seconds - remaining) / window_seconds * 100.0
            out["pace"] = int(round(out["pct"] - elapsed_pct))
    return out


# --------------------------------------------------------------------- consulta
def fetch_usage():
    now = time.time()
    with _lock:
        fresh = _cache["data"] and (now - _cache["ts"] < POLL_MIN_S)
        backing_off = now < _cache["wait_until"]
    if fresh or (backing_off and _cache["data"]):
        return _cache["data"]

    try:
        token = read_access_token()
        req = urllib.request.Request(ANTHROPIC_URL, headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
            "Content-Type": "application/json",
            "User-Agent": "claude-usage-oled-bridge/1.0",
        })
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = json.loads(resp.read().decode("utf-8"))

        five = _window(raw.get("five_hour"), 5 * 3600)
        week = _window(raw.get("seven_day"), 7 * 86400)
        sonnet = raw.get("seven_day_sonnet") or {}
        opus = raw.get("seven_day_opus") or {}

        data = {
            "ok": True,
            "five_hour_pct": five["pct"],
            "five_hour_left": five["left"],
            "five_hour_pace": five["pace"],
            "seven_day_pct": week["pct"],
            "seven_day_left": week["left"],
            "seven_day_pace": week["pace"],
            "sonnet_pct": sonnet.get("utilization"),
            "opus_pct": opus.get("utilization"),
            "updated": datetime.now().strftime("%H:%M"),
        }
        with _lock:
            _cache.update(data=data, ts=time.time(), wait_until=0.0)
        print(f"[{data['updated']}] 5h={data['five_hour_pct']}%  "
              f"7d={data['seven_day_pct']}%")
        return data

    except urllib.error.HTTPError as e:
        if e.code == 401:
            msg = "Token expirado: abra o Claude Code"
        elif e.code == 429:
            msg = "Rate limit (429), aguardando"
            with _lock:
                _cache["wait_until"] = time.time() + BACKOFF_429_S
        else:
            msg = f"HTTP {e.code}"
        return _error_or_stale(msg)
    except Exception as e:
        return _error_or_stale(str(e))


def _error_or_stale(msg):
    print("ERRO:", msg)
    with _lock:
        if _cache["data"]:                 # devolve o ultimo valor bom
            stale = dict(_cache["data"])
            stale["stale"] = True
            return stale
    return {"ok": False, "error": msg}


# ---------------------------------------------------------------------- servidor
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.rstrip("/") in ("", "/usage", "/usage/")  or self.path == "/":
            body = json.dumps(fetch_usage()).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):          # silencia o log padrao
        pass


if __name__ == "__main__":
    print(f"Ponte Claude->ESP32 em http://0.0.0.0:{PORT}/usage")
    print("Deixe rodando; o ESP32 consulta este endereco na rede local.")
    try:
        fetch_usage()                      # primeira consulta ja na partida
        ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrado.")

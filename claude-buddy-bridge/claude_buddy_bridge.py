#!/usr/bin/env python3
"""
claude_buddy_bridge.py — ponte Claude Code CLI ⇄ hardware buddy (BLE NUS)

O firmware ClaudeBuddyOLED fala o protocolo do claude-desktop-buddy
(JSON por linha sobre Nordic UART Service). O app Claude Desktop so
alimenta o buddy com as proprias sessoes; esta ponte faz o mesmo papel
para sessoes do **Claude Code CLI**, alimentada pelos hooks do CLI:

  [hooks do Claude Code] --HTTP 127.0.0.1:8788--> [esta ponte] --BLE--> [ESP32]

Eventos usados: SessionStart/SessionEnd (sessoes), UserPromptSubmit
(running), Stop (idle + celebrate), PreToolUse (transcript + aprovacao
opcional pelo botao da placa), Notification (estado "attention").

Aprovacao pelo dispositivo (opcional): rode com --ask-tools Bash,Write
e o PreToolUse desses tools bloqueia ate o botao da placa decidir
(curto = aprova, longo = nega). Sem resposta em --ask-timeout s, o hook
devolve vazio e o prompt normal do terminal acontece como sempre.

Uso:
  pip3 install bleak
  python3 claude_buddy_bridge.py [--owner SeuNome] [--ask-tools Bash,Write]

Registre os hooks no ~/.claude/settings.json (veja README.md ao lado).
"""

import argparse
import asyncio
import json
import os
import sys
import time
from collections import deque
from datetime import datetime, date

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("instale a dependencia BLE:  pip3 install bleak")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # ponte → dispositivo (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # dispositivo → ponte (notify)

HTTP_HOST, HTTP_PORT = "127.0.0.1", 8788
HEARTBEAT_S = 10
SESSION_TTL_S = 30 * 60          # esquece sessao sem atividade ha 30 min
STATE_FILE = os.path.expanduser("~/.claude-buddy-bridge.json")


def hhmm() -> str:
    return datetime.now().strftime("%H:%M")


def fmt_tk(v: int) -> str:
    if v >= 1_000_000:
        return f"{v/1_000_000:.1f}M"
    if v >= 1000:
        return f"{v/1000:.1f}K"
    return str(v)


# Verbos rotativos do "spinner" da placa (so ASCII — a fonte do OLED nao
# tem acentos). Quando o transcript mostra bloco de thinking, vira
# "Pensando" fixo.
VERBS = ["Matutando", "Cavoucando", "Tramando", "Cozinhando",
         "Maquinando", "Remoendo", "Rabiscando", "Tecendo"]


class State:
    """Estado agregado das sessoes CLI + prompt pendente no dispositivo."""

    def __init__(self) -> None:
        self.sessions: dict[str, dict] = {}      # sid -> {running, last, ...}
        self.entries: deque[str] = deque(maxlen=6)  # mais novo no fim
        self.msg = "CLI conectado"
        self.completed_until = 0.0
        self.note_until = 0.0                    # Notification → attention
        self.pending: dict | None = None         # {id, tool, hint, future}
        self.prompt_seq = 0
        self.prompt_lock = asyncio.Lock()        # 1 pergunta por vez na placa
        self.dirty = asyncio.Event()
        # Contadores persistidos — sobrevivem a restarts da ponte (o
        # "hoje" na placa nao zera toda vez que a ponte religa).
        self.tokens_total = 0
        self.tokens_today = 0
        self.day = date.today()
        try:
            with open(STATE_FILE) as f:
                st = json.load(f)
            self.tokens_total = int(st.get("tokens_total", 0))
            if st.get("day") == self.day.isoformat():
                self.tokens_today = int(st.get("tokens_today", 0))
        except (OSError, ValueError):
            pass

    def save(self) -> None:
        try:
            with open(STATE_FILE, "w") as f:
                json.dump({"tokens_total": self.tokens_total,
                           "tokens_today": self.tokens_today,
                           "day": self.day.isoformat()}, f)
        except OSError:
            pass

    def touch(self, sid: str, running: bool | None = None,
              transcript: str | None = None) -> None:
        s = self.sessions.setdefault(sid, {
            "running": False, "last": 0.0, "transcript": None,
            "offset": 0, "tok_by_id": {}, "last_sum": 0,
            "started": None, "turn_base": 0, "thinking": False,
        })
        s["last"] = time.time()
        if running is not None:
            s["running"] = running
            if not running:
                s["started"] = None
        if transcript:
            s["transcript"] = transcript

    def add_tokens(self, delta: int) -> None:
        today = date.today()
        if today != self.day:
            self.day = today
            self.tokens_today = 0
        self.tokens_total += delta
        self.tokens_today += delta
        self.save()

    # Le as linhas novas do transcript JSONL da sessao e acumula os
    # output_tokens reais das mensagens do assistente (dedup por id da
    # mensagem — chunks de streaming repetem o id com usage crescente).
    def scan_session(self, s: dict) -> bool:
        path = s.get("transcript")
        if not path:
            return False
        try:
            if os.path.getsize(path) <= s["offset"]:
                return False
        except OSError:
            return False
        # Primeira leitura de um transcript ja existente: sincroniza o
        # contador sem creditar — senao cada restart da ponte re-credita
        # o historico inteiro da sessao (e o pet sobe de nivel de graca).
        baseline = s["offset"] == 0
        try:
            with open(path, "r", errors="replace") as f:
                f.seek(s["offset"])
                while True:
                    pos = f.tell()
                    line = f.readline()
                    if not line:
                        break
                    if not line.endswith("\n"):   # linha ainda sendo escrita
                        f.seek(pos)
                        break
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        continue
                    if obj.get("type") != "assistant":
                        continue
                    m = obj.get("message") or {}
                    mid = m.get("id") or obj.get("uuid") or "?"
                    out = (m.get("usage") or {}).get("output_tokens") or 0
                    if out:
                        prev = s["tok_by_id"].get(mid, 0)
                        if out > prev:
                            s["tok_by_id"][mid] = out
                    c = m.get("content")
                    if isinstance(c, list) and c:
                        s["thinking"] = (c[-1].get("type") == "thinking")
                s["offset"] = f.tell()
        except OSError:
            return False
        new_sum = sum(s["tok_by_id"].values())
        delta = new_sum - s["last_sum"]
        if delta <= 0:
            return False
        s["last_sum"] = new_sum
        if baseline:
            s["turn_base"] = new_sum
            return False
        self.add_tokens(delta)
        return True

    def prune(self) -> None:
        now = time.time()
        for sid in [k for k, v in self.sessions.items()
                    if now - v["last"] > SESSION_TTL_S]:
            del self.sessions[sid]

    def snapshot(self) -> dict:
        self.prune()
        now = time.time()
        total = len(self.sessions)
        running = sum(1 for s in self.sessions.values() if s["running"])
        waiting = 1 if (self.pending or now < self.note_until) else 0
        snap = {
            "total": total,
            "running": running,
            "waiting": waiting,
            "msg": (f"approve: {self.pending['tool']}" if self.pending
                    else self.msg)[:23],
            "entries": list(self.entries)[::-1],   # protocolo: mais novo 1o
            "tokens": self.tokens_total,
            "tokens_today": self.tokens_today,
        }
        if now < self.completed_until:
            snap["completed"] = True
        if self.pending:
            snap["prompt"] = {
                "id": self.pending["id"],
                "tool": self.pending["tool"][:18],
                "hint": self.pending["hint"][:42],
            }
        return snap


STATE = State()


# ───────────────────────── lado BLE ─────────────────────────

class Buddy:
    def __init__(self, owner: str | None, name_prefix: str):
        self.owner = owner
        self.name_prefix = name_prefix
        self.client: BleakClient | None = None
        self._rxline = bytearray()

    @property
    def connected(self) -> bool:
        return bool(self.client and self.client.is_connected)

    def _on_notify(self, _h, data: bytearray) -> None:
        for b in data:
            if b in (10, 13):
                if self._rxline:
                    self._handle_line(self._rxline.decode("utf-8", "replace"))
                    self._rxline.clear()
            else:
                self._rxline.append(b)

    def _handle_line(self, line: str) -> None:
        try:
            doc = json.loads(line)
        except ValueError:
            return
        if doc.get("cmd") == "permission" and STATE.pending \
                and doc.get("id") == STATE.pending["id"]:
            decision = "allow" if doc.get("decision") == "once" else "deny"
            print(f"[buddy] decisao do dispositivo: {decision}")
            fut = STATE.pending["future"]
            if not fut.done():
                fut.set_result(decision)
        elif "ack" in doc:
            pass                                   # acks de owner/status etc.

    async def send(self, obj: dict) -> None:
        if not self.connected:
            return
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        try:
            for i in range(0, len(data), 180):     # MTU-3 com folga
                await self.client.write_gatt_char(NUS_RX, data[i:i + 180],
                                                  response=True)
        except Exception as e:                     # noqa: BLE drop no meio
            print(f"[ble] falha no envio: {e}")

    async def run(self) -> None:
        """Loop eterno: procura, conecta, alimenta; reconecta ao cair."""
        while True:
            try:
                dev = await self._find()
                if not dev:
                    await asyncio.sleep(5)
                    continue
                print(f"[ble] conectando em {dev.name} ({dev.address})…")
                async with BleakClient(dev) as client:
                    self.client = client
                    # 1o acesso GATT dispara o pareamento do macOS — a placa
                    # mostra o passkey, o sistema pede para digitar.
                    await client.start_notify(NUS_TX, self._on_notify)
                    print("[ble] conectado")
                    await self._hello()
                    await self._feed()
            except Exception as e:
                print(f"[ble] {type(e).__name__}: {e}")
            finally:
                self.client = None
                if STATE.pending and not STATE.pending["future"].done():
                    STATE.pending["future"].set_result("none")
            print("[ble] desconectado; tentando de novo em 5s")
            await asyncio.sleep(5)

    async def _find(self):
        print(f"[ble] procurando '{self.name_prefix}*'…")
        devs = await BleakScanner.discover(timeout=6.0,
                                           service_uuids=[NUS_SERVICE])
        for d in devs:
            if d.name and d.name.startswith(self.name_prefix):
                return d
        # fallback: alguns adaptadores nao filtram por service uuid
        devs = await BleakScanner.discover(timeout=4.0)
        for d in devs:
            if d.name and d.name.startswith(self.name_prefix):
                return d
        return None

    async def _hello(self) -> None:
        tz = -time.timezone + (3600 if time.localtime().tm_isdst else 0)
        await self.send({"time": [int(time.time()), tz]})
        if self.owner:
            await self.send({"cmd": "owner", "name": self.owner})

    async def _feed(self) -> None:
        while self.connected:
            await self.send(STATE.snapshot())
            try:
                await asyncio.wait_for(STATE.dirty.wait(), HEARTBEAT_S)
            except asyncio.TimeoutError:
                pass
            STATE.dirty.clear()


# ───────────────────── lado hooks (HTTP) ─────────────────────

def summarize_tool(payload: dict) -> str:
    tool = payload.get("tool_name", "?")
    ti = payload.get("tool_input") or {}
    hint = (ti.get("command") or ti.get("file_path") or ti.get("url")
            or ti.get("prompt") or ti.get("description") or "")
    return tool, str(hint).replace("\n", " ")


# Mostra a pergunta na placa e espera o botao (curto=allow, longo=deny).
# Serializado por lock — perguntas simultaneas (varias sessoes) entram em
# fila em vez de passar direto. Retorna "allow", "deny" ou "none".
async def relay_prompt(tool: str, hint: str, timeout: float) -> str:
    deadline = time.time() + timeout
    async with STATE.prompt_lock:
        remain = deadline - time.time()
        if remain <= 1:
            return "none"                        # fila comeu o tempo todo
        STATE.prompt_seq += 1
        fut = asyncio.get_running_loop().create_future()
        STATE.pending = {"id": f"req_cli{STATE.prompt_seq}",
                         "tool": tool, "hint": hint, "future": fut}
        STATE.dirty.set()
        print(f"[hook] aguardando botao da placa para {tool}…")
        try:
            decision = await asyncio.wait_for(fut, remain)
        except asyncio.TimeoutError:
            decision = "none"
        STATE.pending = None
        STATE.note_until = 0
        STATE.dirty.set()
        return decision


async def handle_hook(payload: dict, ask_tools: set[str],
                      ask_timeout: float, buddy: Buddy) -> dict:
    event = payload.get("hook_event_name", "")
    sid = payload.get("session_id", "?")[:8]
    tpath = payload.get("transcript_path")

    if event == "SessionStart":
        STATE.touch(sid, running=False, transcript=tpath)
        STATE.msg = "sessao aberta"
    elif event == "SessionEnd":
        s = STATE.sessions.pop(sid, None)
        if s:
            STATE.scan_session(s)     # ultimos tokens antes de esquecer
        STATE.msg = "sessao encerrada"
    elif event == "UserPromptSubmit":
        STATE.touch(sid, running=True, transcript=tpath)
        s = STATE.sessions[sid]
        s["started"] = time.time()
        s["turn_base"] = s["last_sum"]
        STATE.note_until = 0          # usuario respondeu — attention some
        p = str(payload.get("prompt", ""))[:60].replace("\n", " ")
        STATE.entries.append(f"{hhmm()} {p}")
        STATE.msg = "pensando..."
    elif event == "Stop":
        STATE.touch(sid, running=False, transcript=tpath)
        STATE.note_until = 0          # turno acabou — attention some
        STATE.completed_until = time.time() + 6
        STATE.msg = "turno concluido"
    elif event == "Notification":
        STATE.touch(sid, transcript=tpath)
        STATE.note_until = time.time() + 60
        m = str(payload.get("message", "atencao"))
        low = m.lower()
        if "permission" in low:
            m = "aprovacao no terminal"
        elif "waiting" in low or "input" in low:
            m = "esperando voce..."
        STATE.msg = m[:23]
    elif event == "PermissionRequest":
        # Dispara exatamente quando o terminal mostraria um prompt de
        # permissao — a pergunta real. Vai sempre para a placa quando
        # conectada; sem botao em ask_timeout s, o prompt normal aparece.
        STATE.touch(sid, transcript=tpath)
        tool, hint = summarize_tool(payload)
        if buddy.connected:
            decision = await relay_prompt(tool, hint, ask_timeout)
            if decision in ("allow", "deny"):
                return {"decision": decision}
        return {}
    elif event == "PreToolUse":
        STATE.touch(sid, running=True, transcript=tpath)
        s = STATE.sessions[sid]
        if s["started"] is None:      # sessao ja estava no meio de um turno
            s["started"] = time.time()
            s["turn_base"] = s["last_sum"]
        tool, hint = summarize_tool(payload)
        STATE.entries.append(f"{hhmm()} {tool} {hint}"[:80])
        STATE.msg = f"{tool}..."
        # Modo agressivo opcional (--ask-tools): exige botao para esses
        # tools em TODA chamada, mesmo as que nem perguntariam.
        if tool in ask_tools and buddy.connected:
            decision = await relay_prompt(tool, hint, ask_timeout)
            if decision in ("allow", "deny"):
                return {"decision": decision}
            return {}                              # timeout → fluxo normal
    STATE.dirty.set()
    return {}


async def http_server(ask_tools: set[str], ask_timeout: float, buddy: Buddy):
    async def client_cb(reader: asyncio.StreamReader,
                        writer: asyncio.StreamWriter) -> None:
        try:
            # parse HTTP minimo: linha de request + headers + corpo JSON
            req = await reader.readline()
            clen = 0
            while True:
                line = await reader.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":")[1])
            body = await reader.readexactly(clen) if clen else b"{}"
            resp: dict = {}
            if req.startswith(b"POST /hook"):
                resp = await handle_hook(json.loads(body), ask_tools,
                                         ask_timeout, buddy)
            elif req.startswith(b"GET /status"):
                resp = {"ble": buddy.connected, **STATE.snapshot()}
            out = json.dumps(resp).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json"
                         b"\r\nContent-Length: " + str(len(out)).encode()
                         + b"\r\nConnection: close\r\n\r\n" + out)
            await writer.drain()
        except Exception as e:
            print(f"[http] {type(e).__name__}: {e}")
        finally:
            writer.close()

    server = await asyncio.start_server(client_cb, HTTP_HOST, HTTP_PORT)
    print(f"[http] hooks em http://{HTTP_HOST}:{HTTP_PORT}/hook "
          f"(status: GET /status)")
    async with server:
        await server.serve_forever()


# Status estilo spinner do CLI: "Matutando 23s 708tk". Le os transcripts
# das sessoes ativas a cada 2s, soma tokens reais e atualiza a linha de
# status da placa enquanto algum turno roda.
async def ticker() -> None:
    while True:
        await asyncio.sleep(2)
        now = time.time()
        changed = False
        for s in list(STATE.sessions.values()):
            changed |= STATE.scan_session(s)
        run = [s for s in STATE.sessions.values() if s["running"]]
        if run and not STATE.pending:
            s = max(run, key=lambda x: x["last"])
            el = int(now - (s["started"] or s["last"]))
            el_s = f"{el//60}m{el%60:02d}" if el >= 60 else f"{el}s"
            turn_tk = max(0, s["last_sum"] - s["turn_base"])
            verb = "Pensando" if s["thinking"] else \
                VERBS[int(now / 4) % len(VERBS)]
            # OLED mostra 21 colunas — encurta o verbo antes de cortar
            # os tokens, que sao a parte util.
            rest = f" {el_s} {fmt_tk(turn_tk)}tk"
            msg = verb[:21 - len(rest)] + rest
            if msg != STATE.msg:
                STATE.msg = msg
                changed = True
        if changed:
            STATE.dirty.set()


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--owner", help="seu primeiro nome (aparece na placa)")
    ap.add_argument("--device", default="Claude",
                    help="prefixo do nome BLE (padrao: Claude)")
    ap.add_argument("--ask-tools", default="",
                    help="tools que pedem aprovacao no botao da placa, "
                         "ex.: Bash,Write,Edit (padrao: nenhum)")
    ap.add_argument("--ask-timeout", type=float, default=55,
                    help="segundos esperando o botao antes de cair no "
                         "prompt normal do terminal (padrao: 55)")
    args = ap.parse_args()

    ask_tools = {t.strip() for t in args.ask_tools.split(",") if t.strip()}
    if ask_tools:
        print(f"[cfg] aprovacao pelo dispositivo para: {', '.join(sorted(ask_tools))}")

    buddy = Buddy(args.owner, args.device)
    await asyncio.gather(buddy.run(),
                         http_server(ask_tools, args.ask_timeout, buddy),
                         ticker())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\ntchau!")

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
import sys
import time
from collections import deque
from datetime import datetime

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


def hhmm() -> str:
    return datetime.now().strftime("%H:%M")


class State:
    """Estado agregado das sessoes CLI + prompt pendente no dispositivo."""

    def __init__(self) -> None:
        self.sessions: dict[str, dict] = {}      # sid -> {running, last}
        self.entries: deque[str] = deque(maxlen=6)  # mais novo no fim
        self.msg = "CLI conectado"
        self.completed_until = 0.0
        self.note_until = 0.0                    # Notification → attention
        self.pending: dict | None = None         # {id, tool, hint, future}
        self.prompt_seq = 0
        self.dirty = asyncio.Event()

    def touch(self, sid: str, running: bool | None = None) -> None:
        s = self.sessions.setdefault(sid, {"running": False, "last": 0.0})
        s["last"] = time.time()
        if running is not None:
            s["running"] = running

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


async def handle_hook(payload: dict, ask_tools: set[str],
                      ask_timeout: float, buddy: Buddy) -> dict:
    event = payload.get("hook_event_name", "")
    sid = payload.get("session_id", "?")[:8]

    if event == "SessionStart":
        STATE.touch(sid, running=False)
        STATE.msg = "sessao aberta"
    elif event == "SessionEnd":
        STATE.sessions.pop(sid, None)
        STATE.msg = "sessao encerrada"
    elif event == "UserPromptSubmit":
        STATE.touch(sid, running=True)
        STATE.note_until = 0          # usuario respondeu — attention some
        p = str(payload.get("prompt", ""))[:60].replace("\n", " ")
        STATE.entries.append(f"{hhmm()} {p}")
        STATE.msg = "pensando..."
    elif event == "Stop":
        STATE.touch(sid, running=False)
        STATE.note_until = 0          # turno acabou — attention some
        STATE.completed_until = time.time() + 6
        STATE.msg = "turno concluido"
    elif event == "Notification":
        STATE.touch(sid)
        STATE.note_until = time.time() + 60
        STATE.msg = str(payload.get("message", "atencao"))[:23]
    elif event == "PreToolUse":
        STATE.touch(sid, running=True)
        tool, hint = summarize_tool(payload)
        STATE.entries.append(f"{hhmm()} {tool} {hint}"[:80])
        STATE.msg = f"{tool}..."
        if tool in ask_tools and buddy.connected and not STATE.pending:
            STATE.prompt_seq += 1
            fut = asyncio.get_running_loop().create_future()
            STATE.pending = {"id": f"req_cli{STATE.prompt_seq}",
                             "tool": tool, "hint": hint, "future": fut}
            STATE.dirty.set()
            print(f"[hook] aguardando botao da placa para {tool}…")
            try:
                decision = await asyncio.wait_for(fut, ask_timeout)
            except asyncio.TimeoutError:
                decision = "none"
            STATE.pending = None
            STATE.note_until = 0
            STATE.dirty.set()
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
                         http_server(ask_tools, args.ask_timeout, buddy))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\ntchau!")

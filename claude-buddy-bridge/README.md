# claude-buddy-bridge — Claude Code CLI ⇄ hardware buddy

O app Claude Desktop só alimenta o hardware buddy com as **próprias**
sessões. Esta ponte faz o mesmo papel para o **Claude Code CLI**: fala o
protocolo BLE do buddy (Nordic UART Service, mesmo do desktop) e se
alimenta dos **hooks** do CLI.

```
[hooks do Claude Code] --HTTP 127.0.0.1:8788--> [claude_buddy_bridge.py] --BLE--> [Heltec V3]
```

> Importante: o dispositivo aceita **um** central por vez. Use a ponte
> *ou* o app desktop (Hardware Buddy), não os dois ao mesmo tempo.

## Instalação

```bash
pip3 install bleak
python3 claude_buddy_bridge.py --owner SeuNome
```

Na primeira conexão o macOS pede o **passkey de 6 dígitos** mostrado no
OLED da placa (pareamento criptografado; fica guardado).

## Hooks no Claude Code

Adicione ao `~/.claude/settings.json` (ajuste o caminho):

```json
{
  "hooks": {
    "SessionStart":     [{ "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py" }] }],
    "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py" }] }],
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py" }] }],
    "Stop":             [{ "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py" }] }],
    "Notification":     [{ "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py" }] }],
    "PreToolUse":       [{ "matcher": "*", "hooks": [{ "type": "command", "command": "python3 /Users/ditel-ssi/Documents/claude-esp32-heltec/claude-buddy-bridge/buddy_hook.py", "timeout": 120 }] }]
  }
}
```

Com a ponte **parada**, os hooks saem em silêncio — o CLI funciona como
sempre. Pode deixar configurado permanentemente.

## O que a placa mostra

| Evento no CLI | Na placa |
|---|---|
| Sessão aberta / prompt enviado | pet acorda, "pensando..." |
| Tools executando | transcript na linha de status |
| Turno concluído | pet comemora 🎉 |
| Notification (CLI pedindo permissão no terminal) | pet fica impaciente + LED pisca |
| Sessão fechada | pet volta a dormir |

## Aprovar pelo botão da placa (opcional)

```bash
python3 claude_buddy_bridge.py --owner SeuNome --ask-tools Bash,Write,Edit
```

Com `--ask-tools`, o PreToolUse desses tools **bloqueia** até você
decidir na placa: **curto = aprova**, **longo = nega**. Sem resposta em
55 s (`--ask-timeout`), o hook devolve vazio e o prompt normal do
terminal acontece como sempre — nada trava.

> Nota: o PreToolUse dispara para *toda* chamada desses tools, inclusive
> as que sua configuração de permissões já permitiria sem perguntar.
> Comece com poucas tools (ex.: só `Bash`) para não virar metralhadora
> de prompts.

## Diagnóstico

```bash
curl http://127.0.0.1:8788/status   # estado da ponte + snapshot atual
```

- `"ble": false` → placa fora de alcance/desligada, ou pareada com o
  app desktop (desconecte lá).
- Placa mostra "conectado, sem dados" → BLE ok, hooks não configurados.

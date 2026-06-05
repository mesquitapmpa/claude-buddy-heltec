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
./buddyctl start          # ou: python3 claude_buddy_bridge.py --owner SeuNome
```

Na primeira conexão o macOS pede o **passkey de 6 dígitos** mostrado no
OLED da placa (pareamento criptografado; fica guardado).

## Controle — buddyctl, /pet e frases naturais

`buddyctl start|stop|status|log` gerencia a ponte em background
(pidfile `/tmp/claude-buddy-bridge.pid`, log `/tmp/claude-buddy-bridge.log`).

Dentro do Claude Code:

- **`/pet`** (`~/.claude/commands/pet.md`): `on`/`off`/`status`/`guard`/`log`
- **Frases naturais** (via `~/.claude/CLAUDE.md` global): "liga o pet",
  "desliga o pet", "pet guard"…

A ponte é opt-in: nada liga sozinho ao abrir uma sessão.

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
| Turno rodando | spinner ao vivo: `Matutando 23s 708tk` (verbo rotativo, tempo e tokens reais do turno; "Pensando" durante thinking) |
| Tools executando | transcript na linha de status |
| Turno concluído | pet comemora 🎉 |
| Notification (CLI pedindo permissão no terminal) | pet fica impaciente + LED pisca |
| Sessão fechada | pet volta a dormir |

Os tokens vêm do transcript JSONL de cada sessão (`transcript_path` dos
hooks), somados a cada 2s. Eles alimentam o pet (`tokens`/`tokens_today`
no protocolo → fed/level/50K por nível) e o contador "hoje" persiste em
`~/.claude-buddy-bridge.json` entre restarts — a primeira leitura de um
transcript já existente sincroniza sem creditar o histórico.

## Aprovar pelo botão da placa

**Padrão (hook `PermissionRequest`):** toda pergunta de permissão que o
terminal mostraria vai **automaticamente para a placa** enquanto a ponte
está conectada — igual ao comportamento do app desktop. **Curto =
aprova**, **longo = nega**. Sem resposta em 55 s (`--ask-timeout`), o
hook devolve vazio e o prompt normal do terminal aparece — nada trava.
Tools já permitidos pela sua configuração não perguntam (nem na placa).

**Modo guardião (opcional, agressivo):**

```bash
./buddyctl start --ask-tools Bash,Write,Edit    # ou /pet guard
```

Com `--ask-tools`, o PreToolUse desses tools exige botão em **toda**
chamada, mesmo as que a sua configuração já permitiria sem perguntar.

> Os hooks são globais: vale para **todas** as sessões do CLI. Perguntas
> simultâneas entram em **fila** — uma por vez chega à placa.

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

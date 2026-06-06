# claude-buddy-heltec 🐹

**Porte do [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
(Anthropic) para a Heltec WiFi Kit 32 V3 — com ponte própria para o Claude
Code CLI.**

Um bichinho de mesa em um ESP32-S3 com OLED que acompanha suas sessões do
Claude via Bluetooth LE: dorme quando nada acontece, acorda quando você
trabalha, mostra o que está rodando no estilo do spinner do CLI, fica
impaciente quando há aprovações pendentes — e deixa você **aprovar ou negar
permissões apertando o botão físico da placa**.

```
   /\_/\          ← 18 pets ASCII animados (gato, capivara,
  ( o   o )         dragão, fantasma, axolote…)
  (  w   )
  (")_(")
Matutando 23s 708tk    ← verbo · tempo do turno · tokens reais
2 sess 1! hoje 117.6K  ← sessões · aguardando · tokens do dia
```

O pet **come os tokens reais** das suas sessões (50K por nível, confete no
level-up) e guarda stats em NVS: aprovações, negações, humor, velocidade de
resposta.

## Como funciona

O dispositivo fala o [protocolo aberto](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md)
do hardware buddy (JSON por linha sobre BLE Nordic UART Service, link
criptografado com passkey). Dois jeitos de alimentá-lo:

```
A) App Claude Desktop (macOS/Win)          B) Claude Code CLI (este repo!)
   Developer Mode → Hardware Buddy            hooks → ponte Python → BLE

   [Claude Desktop] ──BLE──> [ESP32]          [hooks do Claude Code]
                                                      │ HTTP 127.0.0.1:8788
                                              [claude_buddy_bridge.py] ──BLE──> [ESP32]
```

O caminho **B** é a novidade deste repo: o app desktop só enxerga as próprias
sessões, então a ponte usa os **hooks do Claude Code** (SessionStart, Stop,
PreToolUse…) para espelhar as sessões do terminal — incluindo um **modo
guardião** em que `Bash`/`Write`/`Edit` bloqueiam até você decidir no botão
da placa (curto = aprova, longo = nega; timeout cai no prompt normal).

## Hardware

- **Heltec WiFi Kit 32 V3** (ESP32-S3, OLED 0,96" 128×64 SSD1306 embutido)
- Cabo USB-C — nada para soldar

## Quickstart

### 1. Firmware

```bash
arduino-cli compile --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED
```

Libs: **Adafruit SSD1306**, **Adafruit GFX**, **ArduinoJson** (core esp32 ≥ 3.x).
Detalhes em [`ClaudeBuddyOLED/README.md`](ClaudeBuddyOLED/README.md).

### 2a. Com o app Claude Desktop

Help → Troubleshooting → **Enable Developer Mode** → Developer →
**Open Hardware Buddy…** → Connect → digite o passkey de 6 dígitos do OLED.

### 2b. Com o Claude Code CLI

```bash
pip3 install bleak
claude-buddy-bridge/buddyctl start
```

Registre os hooks no `~/.claude/settings.json` e (opcional) crie o comando
`/pet` — snippets prontos em
[`claude-buddy-bridge/README.md`](claude-buddy-bridge/README.md). Depois é só
dizer "liga o pet" ou `/pet guard` dentro do Claude Code.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| [`ClaudeBuddyOLED/`](ClaudeBuddyOLED/) | Firmware Heltec V3 (Arduino/ESP32-S3, NimBLE, OLED mono) — **testado no hardware** |
| [`ClaudeBuddyCardputer/`](ClaudeBuddyCardputer/) | Firmware M5Stack Cardputer Adv (colorido 240×135, teclado, IMU, speaker) — compila, aguardando hardware |
| [`claude-buddy-bridge/`](claude-buddy-bridge/) | Ponte CLI: `claude_buddy_bridge.py` (BLE + hooks), `buddy_hook.py`, `buddyctl` — serve os dois firmwares |
| [`ClaudeUsageOLED/`](ClaudeUsageOLED/) | Bônus: monitor de uso do plano Pro/Max no OLED (projeto irmão, com `claude_usage_bridge.py`) |

## Diferenças vs. o original (M5StickCPlus)

| | Original | Este porte |
|---|---|---|
| Display | TFT colorido 135×240 | OLED mono 128×64 |
| Personagens | ASCII + GIF | só ASCII (os 18, inalterados) |
| IMU | shake/face-down | — (placa não tem) |
| Botões | A + B | PRG: curto / longo (700 ms) |
| Stack BLE | Bluedroid | NimBLE (core esp32 3.x no S3) |
| Fonte de dados | só app desktop | app desktop **ou** Claude Code CLI |

## Créditos e licença

- Projeto original: [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
  © 2026 Anthropic, PBC (MIT) — os 18 pets ASCII e o protocolo são de lá.
- Porte Heltec V3 + ponte CLI: © 2026 Esmalie Mesquita (MIT).

Licença: [MIT](LICENSE).

# Claude Buddy — Heltec WiFi Kit 32 V3

Porte do [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
(Anthropic, original para M5StickCPlus) para a **Heltec WiFi Kit 32 V3**
(ESP32-S3, OLED 0,96" 128×64).

Um bichinho de mesa que acompanha suas sessões do Claude via **BLE**
(Nordic UART Service): dorme quando nada acontece, acorda quando você
trabalha, fica impaciente quando há aprovações pendentes — e deixa
**aprovar/negar permissões pelo botão PRG** da placa. 18 pets ASCII
(gato, capivara, dragão, fantasma…).

```
   /\_/\        ← pet ASCII animado (5 fps)
  ( o   o )
  (  w   )
  (")_(")
aprovar? 4s   Bash      ← prompt de permissão
curto=SIM  longo=NAO
```

## Diferenças vs. o original (M5StickCPlus)

| | Original | Este porte |
|---|---|---|
| Display | TFT colorido 135×240 | OLED mono 128×64 |
| Personagens | ASCII + GIF | só ASCII (18 species) |
| IMU | shake/face-down | — (placa não tem) |
| Botões | A + B | PRG: curto / longo (700 ms) |
| Stack BLE | Bluedroid | NimBLE (core esp32 3.x no S3) |

## Build e gravação

Compilar (a partir da pasta do projeto):

```bash
arduino-cli compile --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED
```

Gravar (placa no USB; a porta aparece como `/dev/cu.usbserial-*` ou
`/dev/cu.wchusbserial*`):

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:heltec_wifi_kit_32_V3 ClaudeBuddyOLED
```

Arduino IDE: placa **Heltec WiFi Kit 32(V3)** (core esp32 da Espressif),
libs **Adafruit SSD1306**, **Adafruit GFX** e **ArduinoJson**.

## Pareamento com o Claude Desktop (macOS/Windows)

1. **Help → Troubleshooting → Enable Developer Mode**
2. **Developer → Open Hardware Buddy…** → **Connect**
3. Escolha `Claude-XXXX` na lista
4. A placa mostra um **passkey de 6 dígitos** — digite no desktop

O link fica criptografado (LE Secure Connections) e reconecta sozinho.
Sessões rodadas **no app desktop** alimentam o buddy nativamente.

## Uso com Claude Code CLI (terminal)

O app desktop só enxerga as próprias sessões. Para sessões do **CLI**,
use a ponte em `../claude-buddy-bridge/` — ela fala o mesmo protocolo
BLE e se alimenta dos hooks do Claude Code. Veja o README de lá.

## Botão PRG

| Contexto | Toque curto | Toque longo (700 ms) |
|---|---|---|
| Prompt de permissão | **aprova** | **nega** |
| Tela inicial (pet) | próxima tela | troca o pet |
| Tela stats | próxima tela | liga/desliga modo demo |
| OLED apagado | acorda | acorda |

O **modo demo** cicla cenários falsos (busy/attention/celebrate…) para
testar sem desktop conectado.

## Telas

1. **Home** — pet animado + linha de status
2. **Stats** — level, mood, fed, aprovações/negações, tokens
3. **Info** — nome BT, MAC, link, dicas de pareamento

O OLED desliga após 5 min sem desktop e sem botão (proteção contra
burn-in); qualquer botão/conexão religa.

## Arquivos

- `ClaudeBuddyOLED.ino` — telas, botão, máquina de estados (substitui o `main.cpp` original)
- `ble_bridge.{h,cpp}` — NUS BLE; adaptado para NimBLE (ESP32-S3, core 3.x)
- `buddy.{h,cpp}`, `buddy_common.h` — renderer dos pets em 128×64 mono
- `<species>.cpp` × 18 — **inalterados** do original (só removido o include do M5)
- `data.h` — parser do protocolo; RTC do M5 → `settimeofday`
- `cmds.h` — comandos do desktop (sem push de GIF — sem sentido em OLED mono)
- `stats.h` — stats/level/NVS, inalterado

# Claude Buddy — M5Stack Cardputer Adv

Segundo porte do [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
neste repo — agora para o **M5Stack Cardputer Adv** (StampS3A/ESP32-S3,
ST7789 colorido 240×135, teclado QWERTY 56 teclas, IMU BMI270,
alto-falante).

> **Status: compilado, aguardando o hardware chegar para teste físico.**
> Compila limpo (907 KB), mas pareamento, teclado, IMU e layout ainda não
> foram validados na placa real.

Em relação ao porte Heltec (mono), aqui **voltam** os recursos do
original: pets **coloridos** a 2×, **IMU** (sacudir = tonto, de bruços =
soneca/energia), **beeps** e **bateria** no status. GIFs ficam para a v2
(precisam de teste visual e ajuste de partição).

## Layout (paisagem 240×135)

```
┌──────────────┬────────────────┐
│              │ Buddy          │
│   /\_/\      │ Matutando 23s  │
│  ( o   o )   │ 708tk          │
│  (  w   )    │ 10:42 git push │
│  (")_(")     │ 10:41 yarn test│
│   (2x, cor)  │ 2 sess  1!     │
│              │ hoje 117.6K tk │
└──────────────┴────────────────┘
   pet à esquerda   painel/aprovação
```

## Teclas

| Tecla | Ação |
|---|---|
| `Y` / `Enter` | aprovar prompt |
| `N` / `Del` | negar prompt |
| `Tab` / `Espaço` | próxima tela (home → stats → info) |
| `S` | trocar pet (na home) |
| `D` | modo demo (na tela stats) |
| `B` | brilho |

## Build

```bash
arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer ClaudeBuddyCardputer
arduino-cli upload -p /dev/cu.usbmodem* --fqbn esp32:esp32:m5stack_cardputer ClaudeBuddyCardputer
```

Libs: **M5Cardputer** (≥1.1.1 — traz o driver TCA8418 do teclado do Adv),
**M5Unified**, **M5GFX**, **ArduinoJson**.

## Conectar

Igual aos outros: o dispositivo anuncia `Claude-XXXX` com o protocolo
NUS — funciona com o app **Claude Desktop** (Developer Mode → Hardware
Buddy) **ou** com a ponte CLI deste repo (`claude-buddy-bridge/buddyctl
start`). A ponte não precisa de nenhuma mudança.

## Notas técnicas

- `ble_bridge.{h,cpp}` é o mesmo do porte Heltec (ESP32-S3 = NimBLE no
  core 3.x, com fallback Bluedroid via `#if`)
- `buddy.cpp` mantém a geometria do original (X_CENTER=67, 2×) na metade
  esquerda; `TFT_eSprite` → `M5Canvas` (base `lgfx::LovyanGFX`)
- IMU via `M5.Imu` (global do M5Unified — a classe `M5Cardputer` não
  expõe `Imu`)
- As 18 species são os arquivos do original inalterados (cores reais!)
- v2 (quando o hardware chegar): GIFs via folder push (LittleFS +
  AnimatedGIF, partição `default_8MB`), ajuste fino visual do layout

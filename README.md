# Claude Usage Monitor — Heltec WiFi Kit 32 (V3)

Monitor físico do uso do plano Claude (Pro/Max) no OLED 0,96" da placa, no
estilo do widget de menu: anel **5H** (janela de sessão), anel **SEMANA**
(limite semanal), tempo até o reset e o **ritmo** (ex.: `-33%` = abaixo do
ritmo da janela). O cabeçalho alterna entre `CLAUDE MAX` e `SONNET x%`.

```
CLAUDE  MAX                •
  5H            SEMANA
 ( 6%)          ( 14%)      <- anéis de progresso
3h00 -33%      1d15h -62%
```

## Como funciona

```
[Anthropic /api/oauth/usage] <-- HTTPS -- [ponte Python no PC] <-- HTTP LAN -- [ESP32 + OLED]
```

A **ponte** (`claude_usage_bridge.py`) lê o token OAuth que o **Claude Code**
mantém no seu computador (Keychain no macOS ou `~/.claude/.credentials.json`
no Linux/Windows), consulta o endpoint de uso da Anthropic e publica um JSON
simples na rede local. O ESP32 só consome esse JSON — o token nunca vai para
o microcontrolador.

> O endpoint `api.anthropic.com/api/oauth/usage` é o mesmo usado pelos apps
> de menu da comunidade, mas **não é documentado oficialmente** e pode mudar
> ou aplicar rate limit (HTTP 429). A ponte já consulta no máximo 1x/min e
> recua 5 min quando recebe 429.

## Materiais

- Heltec WiFi Kit 32 **V3** (ESP32-S3, OLED embutido — nada para soldar)
- Cabo USB-C
- PC com Python 3.8+ e **Claude Code logado** na sua conta Pro/Max

## Passo 1 — Ponte no PC

```bash
python3 claude_usage_bridge.py          # porta padrão 8787
```

Teste no navegador: `http://localhost:8787/usage` deve mostrar o JSON.
Anote o IP do PC na rede (ex.: `192.168.1.50`) e libere a porta 8787 no
firewall se necessário. Deixe o script rodando (ou registre como serviço).

## Passo 2 — ESP32 (Arduino IDE)

1. **Preferências → URLs adicionais de placas:**
   `https://resource.heltec.cn/download/package_heltec_esp32_index.json`
2. **Boards Manager:** instale *Heltec ESP32 Series Dev-boards* e selecione a
   placa **WiFi Kit 32(V3)**.
3. **Library Manager:** instale *Heltec ESP32 Dev-Boards* (traz
   `HT_SSD1306Wire.h`) e *ArduinoJson*.
4. Abra `ClaudeUsageOLED/ClaudeUsageOLED.ino`, edite `WIFI_SSID`,
   `WIFI_PASS` e `BRIDGE_URL` (IP do seu PC) e grave na placa.

Pinos do OLED já vêm definidos pela placa V3: `SDA_OLED=17`, `SCL_OLED=18`,
`RST_OLED=21`, `Vext=36` (o sketch coloca `Vext` em LOW para ligar o display).

## Problemas comuns

| Sintoma | Causa provável / solução |
|---|---|
| `Sem dados / ponte offline` | Ponte parada, IP errado em `BRIDGE_URL` ou firewall bloqueando a porta 8787. |
| `Token expirado` | Abra o Claude Code uma vez para renovar as credenciais. |
| `Rate limit (429)` | Normal às vezes; a ponte espera 5 min e segue o último valor. |
| OLED apagado | Confirme placa **WiFi Kit 32(V3)** selecionada (define os pinos e o `Vext`). |
| Display sem o `HT_SSD1306Wire.h` | Instale a lib *Heltec ESP32 Dev-Boards*; alternativa: lib ThingPulse "ESP8266 and ESP32 OLED driver for SSD1306" trocando o construtor. |

## Segurança

- O JSON da ponte fica exposto **só na sua LAN** e contém apenas percentuais
  de uso — sem token, sem conteúdo de conversas.
- Não exponha a porta 8787 para a internet.

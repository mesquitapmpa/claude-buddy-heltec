# Spec Kit + Claude Code — Monitor de uso do Claude no Heltec V3

Roteiro completo: primeiro a **conexão pelo terminal** (placa + WiFi + ponte),
depois os **prompts prontos** para colar no Claude Code com o Spec Kit.

---

## FASE 0 — Estabelecer conexão (só terminal, antes de codificar)

### 0.1 Placa ↔ PC (serial USB)

```bash
# Conecte a Heltec V3 pelo USB-C e descubra a porta:
ls /dev/ttyUSB* /dev/ttyACM*        # Linux
ls /dev/cu.*                        # macOS (CP2102 -> cu.usbserial-XXXX)
# Windows: Gerenciador de Dispositivos -> Portas (COMx)

# Linux: permissão de acesso à porta (relogar depois)
sudo usermod -aG dialout $USER

# "Conversar" com a placa (115200 baud):
screen /dev/ttyUSB0 115200          # sair: Ctrl+A, depois K
```

Se aparecer texto de boot ao apertar RST na placa, a serial está ok.

### 0.2 arduino-cli (é o que o Claude Code vai usar para compilar/gravar)

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://resource.heltec.cn/download/package_heltec_esp32_index.json
arduino-cli core update-index
arduino-cli core search heltec          # instale o core que aparecer
arduino-cli board listall | grep -i kit # anote o FQBN do "WiFi Kit 32(V3)"
arduino-cli lib install ArduinoJson "Heltec ESP32 Dev-Boards"
```

### 0.3 Rede de casa + ponte

```bash
# IP do PC na rede de casa (vai no firmware como BRIDGE_URL):
ipconfig getifaddr en0     # macOS
hostname -I                # Linux
ipconfig                   # Windows

# Ponte rodando e respondendo (Claude Code logado na conta Pro/Max):
python3 claude_usage_bridge.py
curl http://localhost:8787/usage     # deve devolver o JSON de uso
```

Anote: **porta serial**, **FQBN**, **SSID/senha do WiFi de casa**, **IP do PC**.

---

## FASE 1 — Iniciar o projeto com Spec Kit

```bash
uv tool install specify-cli --from git+https://github.com/github/spec-kit.git
specify init claude-usage-monitor --ai claude
cd claude-usage-monitor

# (opcional, recomendado) copie os arquivos de referência já funcionais:
mkdir -p reference
cp ../ClaudeUsageOLED/ClaudeUsageOLED.ino ../claude_usage_bridge.py reference/

claude        # abre o Claude Code dentro do projeto
```

---

## FASE 2 — Prompts (cole na ordem, dentro do Claude Code)

### Prompt 1 — `/speckit.constitution`

```text
/speckit.constitution Princípios deste projeto:
- Firmware compila e grava EXCLUSIVAMENTE via arduino-cli na linha de comando
  (compile, upload, monitor). Nenhuma dependência de IDE gráfica.
- Nenhum segredo no git: credenciais do WiFi de casa e URL da ponte ficam em
  config.h (gitignorado), com um config.example.h versionado como modelo.
- O token OAuth da Anthropic NUNCA sai do computador: o ESP32 só conversa com
  a ponte local via HTTP na LAN.
- Ponte em Python 3 usando SOMENTE biblioteca padrão (zero pip install).
- Respeito a rate limits: no máximo 1 consulta/min ao endpoint da Anthropic e
  backoff de 5 min ao receber HTTP 429, servindo o último valor em cache.
- Resiliência: o dispositivo nunca trava; queda de WiFi ou ponte offline
  exibe estado de erro no display e se recupera sozinho.
- Comentários de código e mensagens de commit em português (pt-BR).
- Toda etapa termina com validação real: compilar com arduino-cli e, quando
  houver placa conectada, gravar e conferir os logs seriais a 115200.
```

### Prompt 2 — `/speckit.specify`

```text
/speckit.specify Quero um monitor físico de mesa que mostra, de relance, o
consumo do meu plano Claude Max, no estilo dos widgets de barra de menu, mas
em um pequeno display dedicado que fica sempre visível ao lado do teclado.

Como funciona para o usuário:
- Ao ligar na tomada/USB, o aparelho se conecta sozinho à rede WiFi da minha
  casa e em poucos segundos passa a exibir os dados de uso, sem nenhuma
  interação. As credenciais da rede são configuráveis sem alterar a lógica.
- A tela mostra simultaneamente dois medidores circulares de progresso:
  (1) SESSÃO: percentual usado da janela corrente de 5 horas;
  (2) SEMANA: percentual usado do limite semanal de 7 dias.
- Dentro/junto de cada medidor aparece o percentual numérico; abaixo, o tempo
  restante até o reset daquela janela (formatos "3h00", "1d15h", "45m").
- Cada janela mostra também um indicador de RITMO: uso% menos tempo
  decorrido% da janela (ex.: "-33%" = gastando abaixo do ritmo; "+12%" =
  acima do ritmo e em risco de estourar antes do reset).
- O cabeçalho alterna a cada poucos segundos entre a identificação do plano
  ("CLAUDE MAX") e o uso semanal específico do modelo Sonnet ("SONNET 4%"),
  quando esse dado existir.
- Um indicador discreto informa se os dados estão atualizados; quando
  qualquer janela atinge 90% ou mais, o LED da placa pisca como alerta.
- Os dados se atualizam automaticamente a cada ~30 segundos.
- Estados de erro são claros e em português: "sem WiFi", "ponte offline",
  "token expirado: abra o Claude Code". O aparelho tenta se recuperar sozinho
  e volta ao normal sem precisar reiniciar.
- Os dados vêm de um pequeno serviço auxiliar que roda no meu computador
  (onde minha conta já está autenticada) e publica apenas percentuais e
  tempos na rede local — nunca o token, nunca conteúdo de conversas.

Critérios de aceitação principais:
1. Energizar o aparelho com a ponte ativa -> medidores corretos em <30 s.
2. Derrubar o WiFi e religar -> o aparelho reconecta e volta a atualizar.
3. Parar a ponte -> mensagem "ponte offline"; religar -> recupera sozinho.
4. Janela >= 90% -> LED piscando enquanto durar a condição.
5. Comparar com /usage do Claude Code -> mesmos percentuais (±1 ponto).
```

### Prompt 3 — `/speckit.plan`

```text
/speckit.plan Stack e restrições técnicas:

HARDWARE: Heltec WiFi Kit 32 V3 (ESP32-S3FN8) com OLED 0,96" 128x64 SSD1306
embutido. Pinos fixos da placa: SDA_OLED=17, SCL_OLED=18, RST_OLED=21,
Vext=36 (colocar em LOW liga a alimentação do display), LED_BUILTIN=35,
botão PRG=GPIO0. Serial a 115200.

FIRMWARE (pasta firmware/): sketch Arduino C++ compilado com arduino-cli.
- Core: pacote Heltec via board manager URL
  https://resource.heltec.cn/download/package_heltec_esp32_index.json.
  Descobrir o FQBN exato com `arduino-cli board listall | grep -i kit`
  e registrá-lo no quickstart.
- Bibliotecas: "Heltec ESP32 Dev-Boards" (classe SSD1306Wire de
  HT_SSD1306Wire.h, fontes ArialMT_Plain_10/16) e ArduinoJson v7.
- WiFi.h em modo STA conectando à rede de casa; credenciais e BRIDGE_URL em
  firmware/config.h (gitignorado) gerado a partir de config.example.h.
- HTTPClient buscando GET <BRIDGE_URL> a cada 30 s, timeout 8 s, parse com
  ArduinoJson, loop não bloqueante baseado em millis().
- Renderização: 2 anéis de progresso desenhados com setPixel/trigonometria
  (trilho pontilhado + arco de 3 px), percentual centralizado, linha
  inferior "tempo_restante ritmo", cabeçalho alternando a cada 4 s.

PONTE (pasta bridge/): claude_usage_bridge.py, Python 3.8+ stdlib apenas.
- Lê o accessToken OAuth do Claude Code: no macOS via
  `security find-generic-password -s "Claude Code-credentials" -w`;
  no Linux/Windows em ~/.claude/.credentials.json
  (campo claudeAiOauth.accessToken).
- Consulta GET https://api.anthropic.com/api/oauth/usage com headers
  Authorization: Bearer <token> e anthropic-beta: oauth-2025-04-20.
  Endpoint NÃO documentado: tratar 401 (token expirado), 429 (backoff 5 min)
  e mudanças de schema sem quebrar. Cache mínimo de 60 s.
- Resposta da Anthropic: five_hour / seven_day / seven_day_sonnet /
  seven_day_opus, cada um com utilization (0-100) e resets_at (ISO 8601).
- Calcula tempo restante formatado e ritmo = utilization - tempo_decorrido%
  da janela (5h=18000 s, 7d=604800 s).
- Servidor http.server na porta 8787 expondo /usage com JSON achatado:
  { ok, five_hour_pct, five_hour_left, five_hour_pace, seven_day_pct,
    seven_day_left, seven_day_pace, sonnet_pct, opus_pct, updated, stale? }

CONTRATO: esse JSON achatado é a interface entre os dois componentes;
documentar em contracts/ e validar com teste que sobe a ponte com resposta
simulada da Anthropic.

REFERÊNCIA: a pasta reference/ contém uma implementação funcional do sketch
e da ponte. Usar como base de comportamento e layout, mas reorganizar
conforme a estrutura deste plano (config.h, pastas, testes).

VALIDAÇÃO: compilar com arduino-cli a cada tarefa de firmware; testes da
ponte com unittest (parsing, formatação de tempo, ritmo, cache/backoff);
gravação e leitura do monitor serial somente com minha confirmação, usando
a porta e o FQBN anotados no quickstart.
```

### Prompt 4 — sequência final

```text
/speckit.tasks
```

Revise a lista gerada, depois:

```text
/speckit.analyze
```

E por fim:

```text
/speckit.implement Placa conectada na porta <SUA_PORTA_SERIAL>, FQBN
<SEU_FQBN>. Pode compilar à vontade; antes de gravar (upload) ou abrir o
monitor serial, me avise. Minha rede de casa: preencherei o config.h eu
mesmo quando você criar o config.example.h.
```

---

## Dicas

- Entre o specify e o plan, vale rodar `/speckit.clarify` para o agente
  apontar ambiguidades da especificação.
- Nunca cole SSID/senha reais nos prompts — eles ficariam no histórico e nos
  arquivos de spec. Deixe o Claude Code criar o `config.example.h` e
  preencha o `config.h` você mesma.
- Se o upload falhar na V3: segure PRG (GPIO0), toque RST, solte PRG e tente
  de novo (modo download manual).

# smart-ai-orologio

Firmware per l'orologio Waveshare ESP32-S3-Touch-AMOLED-2.06, usato come telecomando da polso
per un assistente sul PC.

Nel codice non c'e' nessun segreto: rete, credenziali e PIN si inseriscono sul dispositivo.

## Compilazione

GitHub Actions (`.github/workflows/firmware.yml`), ESP-IDF v5.5.5. Il risultato,
`smart-ai-orologio.bin`, si scrive all'indirizzo `0x0`, per esempio con
https://espressif.github.io/esptool-js/, dopo aver confrontato la sua sha256.

## Origine

Parti di configurazione e di struttura vengono dal repository ufficiale
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (esempio `02_lvgl_demo_v9` e
componenti `VibrationTest`, `ButtonTest`, `Recorder` e `bsp_extra` del firmware di fabbrica, questi ultimi tre
per il tasto BOOT e i microfoni), distribuito con licenza Apache 2.0:
vedi `LICENSE-APACHE-2.0`.

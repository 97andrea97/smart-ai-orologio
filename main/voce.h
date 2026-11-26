// Voce (25/11/2026): microfoni e invio dell'audio al PC a pezzi (PROTOCOLLO.md del PC, "Voce dall'orologio").
//
// Microfoni: come il registratore del firmware di fabbrica Waveshare (examples/factory/components/Recorder e
// bsp_extra/src/bsp_board_extra.c, repository github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06): profilo voce a
// 24 kHz del pacchetto della scheda (bsp_audio_init_voice_24k), ES7210 in TDM a 4 posti, i due microfoni frontali
// (posto 0 = MIC1, posto 2 = MIC2) con guadagno 24 dB. Qui i due posti diventano un solo canale (media).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rete.h"

#define VOCE_HZ 24000
// Durata massima: la stessa di VOCE_OROLOGIO_MAX_S in pc/regole.mjs (60 s = 2,88 MB nella PSRAM).
#define VOCE_MAX_S 60
// Un pezzo: 4 s di audio (192 000 byte, 256 000 in base64), un quarto della busta massima del ponte.
#define VOCE_PEZZO_BYTE (4 * VOCE_HZ * 2)
// Risposta del PC a voce_stato: VOCE_OROLOGIO_MAX_CARATTERI_* in pc/regole.mjs sono calcolati su questo numero.
#define RISPOSTA_VOCE_CAP 8192
// Risposta del PC a voce_audio (26/11/2026): un pezzo di VOCE_PEZZO_BYTE di PCM in base64 piu' il resto del JSON e lo
// zero finale (la prova prove/prova_orologio_voce.mjs del PC controlla che il pezzo piu' grande ci stia). Nella PSRAM.
#define RISPOSTA_AUDIO_CAP (4 * ((VOCE_PEZZO_BYTE + 2) / 3) + 512)
// Volume dell'altoparlante per la risposta letta, da 0 a 100 (esp_codec_dev_set_out_vol). Scelta nostra da provare al
// polso: il firmware di fabbrica parte da 60 (CODEC_DEFAULT_VOLUME, PROBABILE, non riletto qui), un po' di piu' per la strada.
#define VOCE_VOLUME 70
// Sotto questa durata la pressione di BOOT e' per sbaglio e non parte niente.
#define VOCE_MIN_MS 500

// Prepara microfoni e altoparlante la prima volta (poi non fa niente). false se il pacchetto della scheda fallisce.
bool voce_avvia(void);
// L'ultimo passo fallito dell'avvio o della lettura dei microfoni, per lo schermo (vuoto se nessuno).
const char *voce_errore(void);
// Registra in `pcm` (mono, VOCE_HZ, 16 bit) finche' continua() dice true o il posto finisce. Torna i campioni
// scritti, oppure -1 se i microfoni non si aprono o non si leggono.
long voce_registra(int16_t *pcm, size_t max_campioni, bool (*continua)(void));
// Manda la registrazione al PC a pezzi; solo l'ultimo aspetta la risposta, che finisce in `risposta`.
ponte_esito_t voce_spedisci(const int16_t *pcm, size_t campioni, char *risposta, size_t cap);
// Fa sentire `pcm` (mono, VOCE_HZ, 16 bit) dall'altoparlante finche' continua() dice true. Stessa apertura del bus della
// registrazione (uscita e microfoni insieme, come il firmware di fabbrica), con l'uscita accesa. false se non si apre
// o non si scrive (il passo in voce_errore()).
bool voce_riproduci(const int16_t *pcm, size_t campioni, bool (*continua)(void));

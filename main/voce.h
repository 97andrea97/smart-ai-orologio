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
// Campioni gia' scritti in `pcm` dalla registrazione in corso: voce_registra li aggiorna dopo ogni lettura, cosi' un altro
// compito puo' mandarli mentre si registra (26/11/2026, audio a pezzi).
extern volatile size_t voce_campioni_pronti;
// Manda la registrazione al PC a pezzi; solo l'ultimo aspetta la risposta, che finisce in `risposta`.
ponte_esito_t voce_spedisci(const int16_t *pcm, size_t campioni, char *risposta, size_t cap);

// Audio a pezzi mentre registra (26/11/2026, STATO.md del PC "Audio a pezzi dall'orologio"): un pezzo parte appena ci sono
// VOCE_FLUSSO_MIN_MS di audio nuovo (fino a VOCE_PEZZO_BYTE se la rete resta indietro); VOCE_FLUSSO_RISERVA_MS restano
// sempre per il rilascio, cosi' l'ultimo pezzo (fine:true) non e' mai vuoto. Scelte nostre, da misurare al polso: con
// pezzi piu' piccoli al rilascio resta meno audio ma ogni pezzo costa una richiesta (e un contatore salvato).
#define VOCE_FLUSSO_MIN_MS 1000
#define VOCE_FLUSSO_RISERVA_MS 100
// Audio leggero (27/11/2026, STATO.md del PC "Audio leggero e rete dell'orologio", PROTOCOLLO.md "Audio leggero"): IMA ADPCM
// a 4 bit, un quarto del PCM. In salita l'audio passa anche da 24 a 16 kHz (quanto basta alla trascrizione): circa 8 KB/s
// invece di 48. Si usa solo se il PC l'ha annunciato (`"cod":"ima"` in una sua risposta): voce_pc_ima.
#define VOCE_ADPCM_HZ 16000
// Pezzo della risposta letta in ADPCM: 2 s (VOCE_OROLOGIO_AUDIO_PEZZO_CAMPIONI_ADPCM in pc/regole.mjs, letto dalla prova).
#define VOCE_AUDIO_ADPCM_CAMPIONI (2 * VOCE_HZ)
// Byte ADPCM di n campioni: 4 di intestazione (primo campione, indice del passo, 0) piu' mezzo byte per ogni altro campione.
#define VOCE_ADPCM_BYTE(n) ((n) == 0 ? 0 : 4 + (n) / 2)
extern volatile bool voce_pc_ima;
// Decodifica un pezzo ADPCM di `n` campioni in `pcm`. false se la lunghezza o l'intestazione non tornano.
bool voce_adpcm_decodifica(const uint8_t *dati, size_t lung, size_t n, int16_t *pcm);
// Un giro di pezzi: stesso id `r`, indice che cresce, JSON nella PSRAM. Con `ima` i pezzi partono in ADPCM a VOCE_ADPCM_HZ;
// i pezzi intermedi devono allora avere un numero di campioni multiplo di 3 (24 -> 16 kHz senza salti fra i pezzi).
typedef struct {
    char r[9];
    int i;
    char *json;
    size_t cap_json;
    bool ima;
    uint8_t ind;          // indice del passo ADPCM alla fine del pezzo prima
    uint8_t *adpcm;       // pezzo codificato, nella PSRAM
    int16_t *ridotto;     // pezzo a 16 kHz, nella PSRAM
    uint32_t byte_inviati, ms_invio;   // per la banda in salita (telemetria)
} voce_giro_t;
// Prepara un giro (id nuovo, JSON per un pezzo da VOCE_PEZZO_BYTE nella PSRAM). false se manca la memoria.
bool voce_giro_apri(voce_giro_t *g);
// Manda `campioni` (al massimo VOCE_PEZZO_BYTE / 2) come pezzo successivo del giro; con fine=true aspetta la risposta.
ponte_esito_t voce_giro_pezzo(voce_giro_t *g, const int16_t *pcm, size_t campioni, bool fine, char *risposta, size_t cap);
void voce_giro_chiudi(voce_giro_t *g);
// Fa sentire `pcm` (mono, VOCE_HZ, 16 bit) dall'altoparlante finche' continua() dice true. Stessa apertura del bus della
// registrazione (uscita e microfoni insieme, come il firmware di fabbrica), con l'uscita accesa. false se non si apre
// o non si scrive (il passo in voce_errore()).
bool voce_riproduci(const int16_t *pcm, size_t campioni, bool (*continua)(void));
// Prova dell'altoparlante (26/11/2026), indipendente da PC, wifi, ponte e sblocco: un tono di 1000 Hz per 1 s, generato qui,
// con la stessa apertura del bus e lo stesso pilota di voce_riproduci. In `esito` (testo per lo schermo, su piu' righe) il
// codice di ogni passo del pilota, il livello del piedino dell'amplificatore (PA) e alcuni registri del chip ES8311.
// true se tutti i passi del pilota sono andati a buon fine: il suono vero lo giudica chi ascolta. Va chiamata dallo stesso
// compito che registra e riproduce (usano lo stesso buffer e gli stessi codec).
bool voce_prova_suono(char *esito, size_t cap);

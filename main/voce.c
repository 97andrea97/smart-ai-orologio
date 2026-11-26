#include "voce.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "voce";

// Dal registratore di fabbrica (bsp_board_extra.h): l'ES7210 manda MIC1, MIC3 (riferimento), MIC2, MIC4 in quest'ordine;
// i posti letti e i microfoni a cui si da' il guadagno sono due numerazioni diverse.
#define POSTI_TDM          4
#define POSTI_TUTTI        0x0F            // BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK
#define POSTO_MIC1         0
#define POSTO_MIC2         2
#define MIC_FRONTALI       0x03            // BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK (MIC1 | MIC2)
#define GUADAGNO_DB        24.0f           // CODEC_DEFAULT_ADC_VOLUME
#define LETTURA_FRAME      256             // READ_FRAMES del registratore di fabbrica

static esp_codec_dev_handle_t altoparlante = NULL, microfoni = NULL;
// 256 frame x 4 posti x 16 bit = 2 KB: piccolo, in memoria interna come nel registratore di fabbrica.
static int16_t grezzo[LETTURA_FRAME * POSTI_TDM];

// 25/11/2026, prova sul vero: "Microfoni non disponibili" senza sapere dove. Il passo fallito e il motivo vanno a schermo
// (voce_errore), con la memoria: niente cavo per capirlo.
// 26/11/2026: oltre alla memoria interna adatta al DMA (l'I2S ne chiede circa 17 KB in 12 pezzi da 1-2 KB), il suo blocco
// piu' grande, tutta la memoria interna e la PSRAM libere. Si misura dopo che il pacchetto della scheda ha gia' restituito
// cio' che aveva preso: e' la memoria che c'era prima del tentativo.
static char errore[160];
const char *voce_errore(void) { return errore; }
volatile size_t voce_campioni_pronti = 0;
static void segna(const char *passo, esp_err_t e)
{
    snprintf(errore, sizeof errore, "%s: %s\nDMA libera %u, blocco %u\ninterna %u, PSRAM %uk",
             passo, e == ESP_OK ? "fallito" : esp_err_to_name(e),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGE(TAG, "%s", errore);
}

bool voce_avvia(void)
{
    errore[0] = 0;
    if (microfoni) return true;
    // Stessa sequenza di bsp_extra_codec_init: bus a 24 kHz (uscita normale, ingresso TDM), poi altoparlante e microfoni.
    esp_err_t e = bsp_audio_init_voice_24k();
    if (e != ESP_OK) { segna("avvio I2S", e); return false; }
    altoparlante = bsp_audio_codec_speaker_init();
    microfoni = altoparlante ? bsp_audio_codec_microphone_init() : NULL;
    if (!altoparlante || !microfoni) {
        segna(!altoparlante ? "codec altoparlante" : "codec microfoni", ESP_OK);
        bsp_audio_deinit();
        altoparlante = microfoni = NULL;
        return false;
    }
    ESP_LOGI(TAG, "microfoni pronti, DMA libera dopo %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return true;
}

long voce_registra(int16_t *pcm, size_t max_campioni, bool (*continua)(void))
{
    if (!voce_avvia()) return -1;
    // Come bsp_extra_codec_set_voice_fs: prima l'uscita (stereo, I2S_SLOT_MODE_STEREO = 2), poi l'ingresso a 4 posti.
    // Differenza voluta: l'uscita resta muta mentre si registra (niente fruscio nei microfoni).
    esp_codec_dev_sample_info_t uscita = { .sample_rate = VOCE_HZ, .channel = 2, .bits_per_sample = 16 };
    esp_codec_dev_sample_info_t ingresso = {
        .sample_rate = VOCE_HZ, .channel = POSTI_TDM, .channel_mask = POSTI_TUTTI, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(altoparlante, &uscita) != ESP_CODEC_DEV_OK) { segna("apertura uscita", ESP_OK); return -1; }
    esp_codec_dev_set_out_mute(altoparlante, true);
    if (esp_codec_dev_open(microfoni, &ingresso) != ESP_CODEC_DEV_OK) {
        segna("apertura microfoni", ESP_OK);
        esp_codec_dev_close(altoparlante);
        return -1;
    }
    if (esp_codec_dev_set_in_channel_gain(microfoni, MIC_FRONTALI, GUADAGNO_DB) != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "guadagno non impostato");

    size_t n = 0;
    bool ok = true;
    int32_t picco = 0;
    voce_campioni_pronti = 0;
    while (n < max_campioni && continua()) {
        if (esp_codec_dev_read(microfoni, grezzo, sizeof grezzo) != ESP_CODEC_DEV_OK) { segna("lettura", ESP_OK); ok = false; break; }
        for (int f = 0; f < LETTURA_FRAME && n < max_campioni; f++) {
            int32_t m = ((int32_t)grezzo[f * POSTI_TDM + POSTO_MIC1] + grezzo[f * POSTI_TDM + POSTO_MIC2]) / 2;
            pcm[n++] = (int16_t)m;
            if (m < 0) m = -m;
            if (m > picco) picco = m;
        }
        __sync_synchronize();        // i campioni scritti prima del numero che li rende visibili all'altro compito
        voce_campioni_pronti = n;
    }
    // Come bsp_extra_codec_dev_stop: prima l'ingresso, poi l'uscita.
    esp_codec_dev_close(microfoni);
    esp_codec_dev_close(altoparlante);
    // Nel registro solo durata e picco (per capire se i microfoni sentono), mai l'audio.
    ESP_LOGI(TAG, "registrati %u campioni, picco %ld su 32767%s", (unsigned)n, (long)picco, ok ? "" : ", lettura fallita");
    return ok ? (long)n : -1;
}

// Risposta letta (26/11/2026). L'apertura e' quella di voce_registra, che sull'orologio funziona: uscita stereo a VOCE_HZ,
// poi l'ingresso a 4 posti (non letto: i campioni in arrivo si perdono, come nel lettore del firmware di fabbrica, che
// imposta sempre tutte e due le direzioni). Niente memoria nuova: il mono diventa stereo in `grezzo` (2 KB gia' riservati,
// 512 frame), perche' registrare e ascoltare non succedono mai insieme (tutti e due li chiama solo il compito della voce).
#define FRAME_USCITA (LETTURA_FRAME * POSTI_TDM / 2)
// Silenzio in coda prima di chiudere: svuota i buffer DMA dell'uscita (6 x 240 frame, 60 ms a 24 kHz, I2S_CHANNEL_DEFAULT_CONFIG)
// cosi' l'ultima parola non si taglia e non si ripete. Sei giri da 512 frame, circa 128 ms.
#define GIRI_SILENZIO 6

bool voce_riproduci(const int16_t *pcm, size_t campioni, bool (*continua)(void))
{
    if (!voce_avvia()) return false;
    esp_codec_dev_sample_info_t uscita = { .sample_rate = VOCE_HZ, .channel = 2, .bits_per_sample = 16 };
    esp_codec_dev_sample_info_t ingresso = {
        .sample_rate = VOCE_HZ, .channel = POSTI_TDM, .channel_mask = POSTI_TUTTI, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(altoparlante, &uscita) != ESP_CODEC_DEV_OK) { segna("apertura uscita", ESP_OK); return false; }
    esp_codec_dev_set_out_mute(altoparlante, true);
    if (esp_codec_dev_open(microfoni, &ingresso) != ESP_CODEC_DEV_OK) {
        segna("apertura microfoni", ESP_OK);
        esp_codec_dev_close(altoparlante);
        return false;
    }
    if (esp_codec_dev_set_out_vol(altoparlante, VOCE_VOLUME) != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "volume non impostato");
    esp_codec_dev_set_out_mute(altoparlante, false);

    size_t n = 0;
    bool ok = true;
    while (n < campioni && continua()) {
        size_t k = campioni - n < FRAME_USCITA ? campioni - n : FRAME_USCITA;
        for (size_t f = 0; f < k; f++) grezzo[2 * f] = grezzo[2 * f + 1] = pcm[n + f];
        if (esp_codec_dev_write(altoparlante, grezzo, (int)(k * 4)) != ESP_CODEC_DEV_OK) { segna("scrittura", ESP_OK); ok = false; break; }
        n += k;
    }
    memset(grezzo, 0, sizeof grezzo);
    for (int g = 0; ok && g < GIRI_SILENZIO; g++) esp_codec_dev_write(altoparlante, grezzo, (int)sizeof grezzo);
    esp_codec_dev_set_out_mute(altoparlante, true);
    esp_codec_dev_close(microfoni);
    esp_codec_dev_close(altoparlante);
    ESP_LOGI(TAG, "letti %u campioni su %u%s", (unsigned)n, (unsigned)campioni, ok ? "" : ", scrittura fallita");
    return ok;
}

// ---- prova dell'altoparlante (26/11/2026) -----------------------------------------------------------------
// Sul polso la risposta si leggeva ma l'altoparlante restava muto, e non c'era modo di sapere se il difetto stava nella
// scheda o nel percorso della risposta (il successo di voce_riproduci non lascia traccia sullo schermo). Qui un tono fisso,
// senza PC: stessa sequenza di apertura di voce_riproduci (uscita stereo, poi ingresso a 4 posti, volume, mute spento),
// piu' la lettura di cio' che il pilota ha davvero impostato. Il PA e' il piedino dell'amplificatore NS4150B (BSP_POWER_AMP_IO,
// GPIO46): letto come fa il firmware di fabbrica (bsp_extra_codec_pa_is_enabled), cioe' abilitando l'ingresso del piedino
// senza toccare l'uscita, perche' gpio_get_level() con l'ingresso spento torna sempre 0.
#define PROVA_HZ        1000     // il tono; VOCE_HZ / PROVA_HZ campioni per periodo, intero: nessun salto fra un periodo e l'altro
#define PROVA_MS        1000
#define PROVA_AMPIEZZA  20000    // su 32767: circa il 61% del fondo scala
#define PROVA_RAMPA_MS  10       // entrata e uscita dolci: niente "clic" ai due estremi
// Volume del tono (0-100, esp_codec_dev_set_out_vol): quello dell'esempio ES8311 di Waveshare per la stessa scheda
// (08_ES8311.ino, EXAMPLE_VOICE_VOLUME 85), piu' alto dei 70 della risposta letta: la prova deve sentirsi bene.
#define PROVA_VOLUME    85
_Static_assert(VOCE_HZ % PROVA_HZ == 0, "il tono deve avere un numero intero di campioni per periodo");
#define PROVA_PERIODO   (VOCE_HZ / PROVA_HZ)

#define ES8311_REG_MUTE     0x31   // bit 6 e 5: mute del DAC (00 = acceso)
#define ES8311_REG_VOLUME   0x32
#define ES8311_REG_DAC      0x12   // 00 = DAC acceso
#define ES8311_REG_ID1      0xFD
#define ES8311_REG_ID2      0xFE

// Accoda una riga di testo in `buf` senza uscire dal suo posto.
static void aggiungi(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void aggiungi(char *buf, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(buf);
    if (n + 1 >= cap) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf + n, cap - n, fmt, a);
    va_end(a);
}

// Un registro dell'ES8311 come due cifre esadecimali, "--" se non si e' potuto leggere.
static void registro(char *testo, int reg)
{
    int v = -1;
    if (esp_codec_dev_read_reg(altoparlante, reg, &v) != ESP_CODEC_DEV_OK || v < 0) strcpy(testo, "--");
    else snprintf(testo, 3, "%02x", v & 0xFF);
}

bool voce_prova_suono(char *esito, size_t cap)
{
    esito[0] = 0;
    if (!voce_avvia()) {
        aggiungi(esito, cap, "Suono NON inviato\nbus: %s", voce_errore());
        return false;
    }
    esp_codec_dev_sample_info_t uscita = { .sample_rate = VOCE_HZ, .channel = 2, .bits_per_sample = 16 };
    esp_codec_dev_sample_info_t ingresso = {
        .sample_rate = VOCE_HZ, .channel = POSTI_TDM, .channel_mask = POSTI_TUTTI, .bits_per_sample = 16,
    };
    const int r_uscita = esp_codec_dev_open(altoparlante, &uscita);
    if (r_uscita != ESP_CODEC_DEV_OK) {
        aggiungi(esito, cap, "Suono NON inviato\napertura uscita: %d %s", r_uscita, esp_err_to_name((esp_err_t)r_uscita));
        return false;
    }
    esp_codec_dev_set_out_mute(altoparlante, true);
    const int r_mic = esp_codec_dev_open(microfoni, &ingresso);
    if (r_mic != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(altoparlante);
        aggiungi(esito, cap, "Suono NON inviato\napertura microfoni: %d %s", r_mic, esp_err_to_name((esp_err_t)r_mic));
        return false;
    }
    const int r_vol = esp_codec_dev_set_out_vol(altoparlante, PROVA_VOLUME);
    const int r_mute = esp_codec_dev_set_out_mute(altoparlante, false);

    // Cio' che il pilota ha lasciato: piedino dell'amplificatore e registri del chip, letti con l'uscita aperta e il mute spento.
    gpio_input_enable(BSP_POWER_AMP_IO);
    const int pa = gpio_get_level(BSP_POWER_AMP_IO);
    char id1[4], id2[4], r_m[4], r_v[4], r_d[4];
    registro(id1, ES8311_REG_ID1);
    registro(id2, ES8311_REG_ID2);
    registro(r_m, ES8311_REG_MUTE);
    registro(r_v, ES8311_REG_VOLUME);
    registro(r_d, ES8311_REG_DAC);

    int16_t onda[PROVA_PERIODO];   // un periodo del tono
    for (int i = 0; i < PROVA_PERIODO; i++) onda[i] = (int16_t)(sinf(6.2831853f * (float)i / (float)PROVA_PERIODO) * PROVA_AMPIEZZA);
    const size_t totale = (size_t)VOCE_HZ * PROVA_MS / 1000;
    const size_t rampa = (size_t)VOCE_HZ * PROVA_RAMPA_MS / 1000;
    size_t n = 0;
    int r_scrittura = ESP_CODEC_DEV_OK;
    while (n < totale && r_scrittura == ESP_CODEC_DEV_OK) {
        const size_t k = totale - n < FRAME_USCITA ? totale - n : FRAME_USCITA;
        for (size_t f = 0; f < k; f++) {
            const size_t i = n + f;
            const size_t bordo = i < totale - 1 - i ? i : totale - 1 - i;   // distanza dall'estremo piu' vicino
            int32_t s = onda[i % PROVA_PERIODO];
            if (bordo < rampa) s = s * (int32_t)bordo / (int32_t)rampa;
            grezzo[2 * f] = grezzo[2 * f + 1] = (int16_t)s;
        }
        r_scrittura = esp_codec_dev_write(altoparlante, grezzo, (int)(k * 4));
        if (r_scrittura == ESP_CODEC_DEV_OK) n += k;
    }
    memset(grezzo, 0, sizeof grezzo);
    for (int g = 0; r_scrittura == ESP_CODEC_DEV_OK && g < GIRI_SILENZIO; g++) esp_codec_dev_write(altoparlante, grezzo, (int)sizeof grezzo);
    esp_codec_dev_set_out_mute(altoparlante, true);
    esp_codec_dev_close(microfoni);
    esp_codec_dev_close(altoparlante);

    const bool ok = r_vol == ESP_CODEC_DEV_OK && r_mute == ESP_CODEC_DEV_OK && r_scrittura == ESP_CODEC_DEV_OK && n == totale;
    if (ok) aggiungi(esito, cap, "Suono inviato: %d Hz per %d s\n", PROVA_HZ, PROVA_MS / 1000);
    else aggiungi(esito, cap, "Suono NON inviato del tutto (%u di %u campioni)\n", (unsigned)n, (unsigned)totale);
    aggiungi(esito, cap, "pilota: uscita %d, mic %d, volume %d, mute %d, scrittura %d\n", r_uscita, r_mic, r_vol, r_mute, r_scrittura);
    aggiungi(esito, cap, "amplificatore (PA, piedino 46): %d\n", pa);
    aggiungi(esito, cap, "ES8311 id %s %s, mute %s, volume %s, dac %s\n", id1, id2, r_m, r_v, r_d);
    aggiungi(esito, cap, "Se non senti niente il difetto e' qui (altoparlante o amplificatore), non nel PC.");
    ESP_LOGI(TAG, "prova suono: pilota %d %d %d %d %d, PA %d, ES8311 id %s %s mute %s volume %s dac %s, campioni %u di %u",
             r_uscita, r_mic, r_vol, r_mute, r_scrittura, pa, id1, id2, r_m, r_v, r_d, (unsigned)n, (unsigned)totale);
    return ok;
}

bool voce_giro_apri(voce_giro_t *g)
{
    // Il pezzo in base64 (4 caratteri ogni 3 byte) dentro il JSON; nella PSRAM, come la busta (rete.c).
    g->cap_json = 4 * ((VOCE_PEZZO_BYTE + 2) / 3) + 160;
    g->json = heap_caps_malloc(g->cap_json, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g->json) { ESP_LOGE(TAG, "memoria per il pezzo"); return false; }
    snprintf(g->r, sizeof g->r, "%08lx", (unsigned long)esp_random());   // id della registrazione, non un segreto
    g->i = 0;
    return true;
}

void voce_giro_chiudi(voce_giro_t *g)
{
    free(g->json);
    g->json = NULL;
}

ponte_esito_t voce_giro_pezzo(voce_giro_t *g, const int16_t *pcm, size_t campioni, bool fine, char *risposta, size_t cap)
{
    const size_t lung = campioni * 2;
    if (!g->json || lung == 0 || lung > VOCE_PEZZO_BYTE) return PONTE_ERRORE;
    int h = snprintf(g->json, g->cap_json, "{\"t\":\"voce\",\"r\":\"%s\",\"i\":%d,\"fine\":%s,\"hz\":%d,\"pcm\":\"",
                     g->r, g->i, fine ? "true" : "false", VOCE_HZ);
    size_t scritti = 0;
    if (h < 0 || mbedtls_base64_encode((unsigned char *)g->json + h, g->cap_json - (size_t)h - 3, &scritti,
                                       (const unsigned char *)pcm, lung) != 0) return PONTE_ERRORE;
    memcpy(g->json + h + scritti, "\"}", 3);
    ponte_esito_t esito = ponte_manda(g->json, (size_t)h + scritti + 2, fine, risposta, cap);
    if (esito != PONTE_OK) ESP_LOGW(TAG, "pezzo %d%s: %s", g->i + 1, fine ? " (ultimo)" : "", ponte_esito_testo(esito));
    else g->i++;
    return esito;
}

ponte_esito_t voce_spedisci(const int16_t *pcm, size_t campioni, char *risposta, size_t cap)
{
    if (campioni == 0) return PONTE_ERRORE;
    voce_giro_t g;
    if (!voce_giro_apri(&g)) return PONTE_ERRORE;
    const size_t per_pezzo = VOCE_PEZZO_BYTE / 2;
    ponte_esito_t esito = PONTE_ERRORE;
    for (size_t da = 0; da < campioni; da += per_pezzo) {
        const size_t n = campioni - da < per_pezzo ? campioni - da : per_pezzo;
        esito = voce_giro_pezzo(&g, pcm + da, n, da + n >= campioni, risposta, cap);
        if (esito != PONTE_OK) break;
    }
    voce_giro_chiudi(&g);
    return esito;
}

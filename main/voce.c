#include "voce.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

bool voce_avvia(void)
{
    if (microfoni) return true;
    // Stessa sequenza di bsp_extra_codec_init: bus a 24 kHz (uscita normale, ingresso TDM), poi altoparlante e microfoni.
    esp_err_t e = bsp_audio_init_voice_24k();
    if (e != ESP_OK) { ESP_LOGE(TAG, "bsp_audio_init_voice_24k: %s", esp_err_to_name(e)); return false; }
    altoparlante = bsp_audio_codec_speaker_init();
    microfoni = altoparlante ? bsp_audio_codec_microphone_init() : NULL;
    if (!altoparlante || !microfoni) {
        ESP_LOGE(TAG, "codec non pronto (altoparlante %d, microfoni %d)", altoparlante != NULL, microfoni != NULL);
        bsp_audio_deinit();
        altoparlante = microfoni = NULL;
        return false;
    }
    ESP_LOGI(TAG, "microfoni pronti");
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
    if (esp_codec_dev_open(altoparlante, &uscita) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "apertura uscita"); return -1; }
    esp_codec_dev_set_out_mute(altoparlante, true);
    if (esp_codec_dev_open(microfoni, &ingresso) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "apertura microfoni");
        esp_codec_dev_close(altoparlante);
        return -1;
    }
    if (esp_codec_dev_set_in_channel_gain(microfoni, MIC_FRONTALI, GUADAGNO_DB) != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "guadagno non impostato");

    size_t n = 0;
    bool ok = true;
    int32_t picco = 0;
    while (n < max_campioni && continua()) {
        if (esp_codec_dev_read(microfoni, grezzo, sizeof grezzo) != ESP_CODEC_DEV_OK) { ok = false; break; }
        for (int f = 0; f < LETTURA_FRAME && n < max_campioni; f++) {
            int32_t m = ((int32_t)grezzo[f * POSTI_TDM + POSTO_MIC1] + grezzo[f * POSTI_TDM + POSTO_MIC2]) / 2;
            pcm[n++] = (int16_t)m;
            if (m < 0) m = -m;
            if (m > picco) picco = m;
        }
    }
    // Come bsp_extra_codec_dev_stop: prima l'ingresso, poi l'uscita.
    esp_codec_dev_close(microfoni);
    esp_codec_dev_close(altoparlante);
    // Nel registro solo durata e picco (per capire se i microfoni sentono), mai l'audio.
    ESP_LOGI(TAG, "registrati %u campioni, picco %ld su 32767%s", (unsigned)n, (long)picco, ok ? "" : ", lettura fallita");
    return ok ? (long)n : -1;
}

ponte_esito_t voce_spedisci(const int16_t *pcm, size_t campioni, char *risposta, size_t cap)
{
    const size_t byte = campioni * 2;
    if (byte == 0) return PONTE_ERRORE;
    // Il pezzo in base64 (4 caratteri ogni 3 byte) dentro il JSON; nella PSRAM, come la busta (rete.c).
    const size_t cap_json = 4 * ((VOCE_PEZZO_BYTE + 2) / 3) + 160;
    char *json = heap_caps_malloc(cap_json, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) { ESP_LOGE(TAG, "memoria per il pezzo"); return PONTE_ERRORE; }
    char reg[9];
    snprintf(reg, sizeof reg, "%08lx", (unsigned long)esp_random());   // id della registrazione, non un segreto

    const int n_pezzi = (int)((byte + VOCE_PEZZO_BYTE - 1) / VOCE_PEZZO_BYTE);
    ponte_esito_t esito = PONTE_ERRORE;
    for (int i = 0; i < n_pezzi; i++) {
        const size_t da = (size_t)i * VOCE_PEZZO_BYTE;
        const size_t lung = byte - da < VOCE_PEZZO_BYTE ? byte - da : VOCE_PEZZO_BYTE;
        const bool fine = i == n_pezzi - 1;
        int h = snprintf(json, cap_json, "{\"t\":\"voce\",\"r\":\"%s\",\"i\":%d,\"fine\":%s,\"hz\":%d,\"pcm\":\"",
                         reg, i, fine ? "true" : "false", VOCE_HZ);
        size_t scritti = 0;
        if (h < 0 || mbedtls_base64_encode((unsigned char *)json + h, cap_json - (size_t)h - 3, &scritti,
                                           (const unsigned char *)pcm + da, lung) != 0) {
            esito = PONTE_ERRORE;
            break;
        }
        memcpy(json + h + scritti, "\"}", 3);
        esito = ponte_manda(json, (size_t)h + scritti + 2, fine, risposta, cap);
        if (esito != PONTE_OK) { ESP_LOGW(TAG, "pezzo %d di %d: %s", i + 1, n_pezzi, ponte_esito_testo(esito)); break; }
    }
    free(json);
    return esito;
}

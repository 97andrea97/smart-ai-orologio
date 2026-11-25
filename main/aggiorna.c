#include "aggiorna.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "aggiorna";

#define PEZZO          4096
#define MAX_RIMANDI    5      // GitHub rimanda il file a un altro indirizzo: uno basta, cinque sono il tetto

const char *aggiorna_esito_testo(aggiorna_esito_t e)
{
    switch (e) {
        case AGGIORNA_OK: return "Aggiornato";
        case AGGIORNA_DATI: return "Dati dell'aggiornamento non validi";
        case AGGIORNA_RETE: return "Scaricamento non riuscito";
        case AGGIORNA_IMPRONTA: return "File diverso da quello atteso: scartato";
        default: return "Errore nello scrivere l'aggiornamento";
    }
}

static bool esa_valida(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static bool rimando(int stato) { return stato == 301 || stato == 302 || stato == 303 || stato == 307 || stato == 308; }

aggiorna_esito_t aggiorna_scarica(const char *url, size_t lunghezza, const char *sha256_hex,
                                  void (*avanzamento)(int percento))
{
    const esp_partition_t *posto = esp_ota_get_next_update_partition(NULL);
    if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) > 300) return AGGIORNA_DATI;
    if (!esa_valida(sha256_hex, 64) || lunghezza == 0) return AGGIORNA_DATI;
    if (!posto || lunghezza > posto->size) return AGGIORNA_DATI;

    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,      // l'indirizzo a cui GitHub rimanda e' lungo
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) return AGGIORNA_RETE;

    aggiorna_esito_t esito = AGGIORNA_RETE;
    uint8_t *buf = malloc(PEZZO);
    esp_ota_handle_t ota = 0;
    bool ota_aperto = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (!buf) goto fine;

    int stato = -1;
    int64_t dichiarata = -1;
    for (int giro = 0; giro <= MAX_RIMANDI; giro++) {
        if (esp_http_client_open(cl, 0) != ESP_OK) goto fine;
        dichiarata = esp_http_client_fetch_headers(cl);
        stato = esp_http_client_get_status_code(cl);
        if (!rimando(stato)) break;
        esp_http_client_flush_response(cl, NULL);
        if (esp_http_client_set_redirection(cl) != ESP_OK) goto fine;
        esp_http_client_close(cl);
    }
    if (stato != 200) { ESP_LOGW(TAG, "risposta %d", stato); goto fine; }
    if (dichiarata > 0 && (size_t)dichiarata != lunghezza) { esito = AGGIORNA_IMPRONTA; goto fine; }

    if (esp_ota_begin(posto, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) { esito = AGGIORNA_MEMORIA; goto fine; }
    ota_aperto = true;
    mbedtls_sha256_starts(&sha, 0);

    size_t letti = 0;
    int ultimo = -1;
    for (;;) {
        int n = esp_http_client_read(cl, (char *)buf, PEZZO);
        if (n < 0) goto fine;
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(cl)) break;
            goto fine;
        }
        if (letti + (size_t)n > lunghezza) { esito = AGGIORNA_IMPRONTA; goto fine; }
        mbedtls_sha256_update(&sha, buf, (size_t)n);
        if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) { esito = AGGIORNA_MEMORIA; goto fine; }
        letti += (size_t)n;
        int p = (int)(letti * 100 / lunghezza);
        if (avanzamento && p / 10 != ultimo) { ultimo = p / 10; avanzamento(p); }
    }
    if (letti != lunghezza) { esito = AGGIORNA_IMPRONTA; goto fine; }

    uint8_t impronta[32];
    char hex[65];
    mbedtls_sha256_finish(&sha, impronta);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", impronta[i]);
    if (strncasecmp(hex, sha256_hex, 64) != 0) { esito = AGGIORNA_IMPRONTA; goto fine; }

    ota_aperto = false;
    if (esp_ota_end(ota) != ESP_OK) { esito = AGGIORNA_MEMORIA; goto fine; }   // controlla anche l'immagine
    if (esp_ota_set_boot_partition(posto) != ESP_OK) { esito = AGGIORNA_MEMORIA; goto fine; }
    ESP_LOGI(TAG, "programma nuovo scritto in %s e verificato", posto->label);
    esito = AGGIORNA_OK;

fine:
    if (ota_aperto) esp_ota_abort(ota);
    mbedtls_sha256_free(&sha);
    free(buf);
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return esito;
}

bool aggiorna_in_prova(void)
{
    esp_ota_img_states_t s;
    const esp_partition_t *corrente = esp_ota_get_running_partition();
    return corrente && esp_ota_get_state_partition(corrente, &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
}

void aggiorna_conferma(void)
{
    if (!aggiorna_in_prova()) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "programma nuovo dichiarato buono");
}

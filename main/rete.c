#include "rete.h"
#include "memoria.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/gcm.h"

static const char *TAG = "rete";

#define BIT_COLLEGATA BIT0
#define DIR_OROLOGIO 0x4F
#define DIR_PC       0x50
#define MAX_BUSTA    4096
#define ATTESA_RISPOSTA_MS 25000
#define PASSO_RISPOSTA_MS  500

static EventGroupHandle_t eventi;
static SemaphoreHandle_t scambio;            // un messaggio per volta, come il PC
static rete_t reti[MAX_RETI];
static int n_reti = 0, rete_corrente = 0;
static volatile rete_fase_t fase = RETE_SPENTA;

static void prova_rete(int i)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, reti[i].ssid, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, reti[i].password, sizeof wc.sta.password - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    fase = RETE_IN_COLLEGAMENTO;
    esp_wifi_connect();
}

static void evento(void *arg, esp_event_base_t base, int32_t id, void *dati)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (n_reti > 0) prova_rete(rete_corrente);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(eventi, BIT_COLLEGATA);
        // Il motivo del rifiuto va nel registro (Console di esptool-js): 15 = password sbagliata,
        // 201 = rete non trovata, 210 = protezione non compatibile (codici wifi_err_reason_t di ESP-IDF).
        if (dati) ESP_LOGW(TAG, "wifi rifiutato da %s, motivo %d", reti[rete_corrente].ssid,
                           ((wifi_event_sta_disconnected_t *)dati)->reason);
        if (n_reti == 0) { fase = RETE_NESSUNA_SALVATA; return; }
        rete_corrente = (rete_corrente + 1) % n_reti;   // la prossima rete salvata, a giro
        prova_rete(rete_corrente);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        fase = RETE_COLLEGATA;
        ESP_LOGI(TAG, "collegato a %s", reti[rete_corrente].ssid);
        xEventGroupSetBits(eventi, BIT_COLLEGATA);
    }
    (void)dati;
}

void rete_avvia(void)
{
    eventi = xEventGroupCreate();
    scambio = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &evento, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &evento, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // le reti stanno solo nel nostro spazio NVS
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    n_reti = memoria_reti(reti);
    fase = n_reti ? RETE_IN_COLLEGAMENTO : RETE_NESSUNA_SALVATA;
    ESP_ERROR_CHECK(esp_wifi_start());
}

void rete_ricarica(void)
{
    n_reti = memoria_reti(reti);
    rete_corrente = 0;
    esp_wifi_disconnect();   // l'evento di disconnessione fa ripartire il giro con le reti nuove
    if (n_reti == 0) fase = RETE_NESSUNA_SALVATA;
    else prova_rete(0);
}

rete_fase_t rete_fase(void) { return fase; }
const char *rete_nome(void) { return n_reti ? reti[rete_corrente].ssid : ""; }

bool rete_attendi(int ms)
{
    return xEventGroupWaitBits(eventi, BIT_COLLEGATA, pdFALSE, pdTRUE, pdMS_TO_TICKS(ms)) & BIT_COLLEGATA;
}

// ---- buste ---------------------------------------------------------------------------------------

static void nonce_di(uint8_t n[12], uint8_t direzione, uint64_t contatore)
{
    memset(n, 0, 12);
    n[0] = direzione;
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(contatore >> (56 - 8 * i));
}

static int sigilla(const uint8_t chiave[32], uint64_t contatore, const char *json, uint8_t *fuori, size_t cap)
{
    size_t lung = strlen(json);
    if (12 + lung + 16 > cap) return -1;
    nonce_di(fuori, DIR_OROLOGIO, contatore);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int e = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, chiave, 256);
    if (e == 0) e = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, lung, fuori, 12, NULL, 0,
                                              (const uint8_t *)json, fuori + 12, 16, fuori + 12 + lung);
    mbedtls_gcm_free(&g);
    return e == 0 ? (int)(12 + lung + 16) : -1;
}

// Apre una busta del PC: direzione giusta, contatore mai visto, sigillo valido. JSON in `testo`.
static bool apri(const uint8_t chiave[32], const uint8_t *busta, int lung, char *testo, size_t cap)
{
    if (lung < 29 || busta[0] != DIR_PC) return false;
    uint64_t contatore = 0;
    for (int i = 0; i < 8; i++) contatore = (contatore << 8) | busta[4 + i];
    if (contatore <= memoria_visto_pc()) return false;      // ripetuta
    size_t corpo = (size_t)lung - 28;
    if (corpo + 1 > cap) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int e = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, chiave, 256);
    if (e == 0) e = mbedtls_gcm_auth_decrypt(&g, corpo, busta, 12, NULL, 0, busta + lung - 16, 16,
                                             busta + 12, (uint8_t *)testo);
    mbedtls_gcm_free(&g);
    if (e != 0) return false;
    testo[corpo] = 0;
    memoria_imposta_visto_pc(contatore);
    return true;
}

// ---- HTTP verso il ponte -------------------------------------------------------------------------

// Una richiesta: torna il codice HTTP (o -1) e mette il corpo della risposta in `fuori`.
static int chiama(const char *url, const char *gettone, esp_http_client_method_t metodo,
                  const uint8_t *corpo, int lung, uint8_t *fuori, int cap, int *letti)
{
    char autorizzazione[80];
    snprintf(autorizzazione, sizeof autorizzazione, "Bearer %s", gettone);
    esp_http_client_config_t c = {
        .url = url,
        .method = metodo,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) return -1;
    esp_http_client_set_header(cl, "Authorization", autorizzazione);
    if (corpo) esp_http_client_set_header(cl, "Content-Type", "application/octet-stream");
    int stato = -1;
    *letti = 0;
    if (esp_http_client_open(cl, corpo ? lung : 0) == ESP_OK) {
        if (!corpo || esp_http_client_write(cl, (const char *)corpo, lung) == lung) {
            esp_http_client_fetch_headers(cl);
            stato = esp_http_client_get_status_code(cl);
            int n = esp_http_client_read_response(cl, (char *)fuori, cap);
            *letti = n > 0 ? n : 0;
        }
    }
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return stato;
}

const char *ponte_esito_testo(ponte_esito_t e)
{
    switch (e) {
        case PONTE_OK: return "ok";
        case PONTE_NON_ABBINATO: return "Orologio non abbinato";
        case PONTE_SENZA_RETE: return "Nessuna rete wifi";
        case PONTE_PC_SPENTO: return "Il PC non e' collegato";
        case PONTE_RIFIUTATO: return "Orologio non riconosciuto: va riabbinato";
        case PONTE_NESSUNA_RISPOSTA: return "Il PC non ha risposto";
        default: return "Errore di rete";
    }
}

ponte_esito_t ponte_scambia(const char *json, char *risposta, size_t cap)
{
    char base[160], gettone[65], url[220];
    uint8_t chiave[32];
    if (!memoria_ponte(base, sizeof base) || !memoria_gettone(gettone, sizeof gettone) || !memoria_chiave(chiave)) {
        return PONTE_NON_ABBINATO;
    }
    if (!rete_attendi(20000)) return PONTE_SENZA_RETE;

    xSemaphoreTake(scambio, portMAX_DELAY);
    ponte_esito_t esito = PONTE_ERRORE;
    uint8_t *busta = malloc(MAX_BUSTA);
    uint8_t *dati = malloc(MAX_BUSTA);
    if (!busta || !dati) goto fine;

    uint64_t contatore = memoria_contatore_nuovo();   // salvato prima di spedire
    if (contatore == 0) goto fine;
    int lung = sigilla(chiave, contatore, json, busta, MAX_BUSTA);
    if (lung < 0) goto fine;

    int letti = 0;
    snprintf(url, sizeof url, "%s/o/invia", base);
    int stato = chiama(url, gettone, HTTP_METHOD_POST, busta, lung, dati, MAX_BUSTA - 1, &letti);
    if (stato == 503) { esito = PONTE_PC_SPENTO; goto fine; }
    if (stato == 401) { esito = PONTE_RIFIUTATO; goto fine; }
    if (stato != 200) { ESP_LOGW(TAG, "/o/invia: %d", stato); goto fine; }
    dati[letti] = 0;
    // {"id":"<uuid>"}: basta trovare il valore fra virgolette dopo "id"
    char *p = strstr((char *)dati, "\"id\"");
    char id[64] = { 0 };
    if (!p || sscanf(p, "\"id\":\"%63[^\"]\"", id) != 1) goto fine;

    snprintf(url, sizeof url, "%s/o/risposta?id=%s", base, id);
    esito = PONTE_NESSUNA_RISPOSTA;
    for (int t = 0; t < ATTESA_RISPOSTA_MS; t += PASSO_RISPOSTA_MS) {
        stato = chiama(url, gettone, HTTP_METHOD_GET, NULL, 0, dati, MAX_BUSTA, &letti);
        if (stato == 200) {
            esito = apri(chiave, dati, letti, risposta, cap) ? PONTE_OK : PONTE_ERRORE;
            break;
        }
        if (stato == 401) { esito = PONTE_RIFIUTATO; break; }
        if (stato != 204) { esito = PONTE_ERRORE; break; }
        vTaskDelay(pdMS_TO_TICKS(PASSO_RISPOSTA_MS));
    }
fine:
    free(busta);
    free(dati);
    memset(chiave, 0, sizeof chiave);
    xSemaphoreGive(scambio);
    return esito;
}

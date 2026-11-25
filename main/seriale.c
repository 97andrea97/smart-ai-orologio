#include "seriale.h"
#include "memoria.h"
#include "rete.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "seriale";
static void (*avvisa)(void) = NULL;

#define MAX_RIGA 1024

static void rispondi(cJSON *r)
{
    char *testo = cJSON_PrintUnformatted(r);
    if (testo) { printf("@@%s\n", testo); fflush(stdout); cJSON_free(testo); }
    cJSON_Delete(r);
}

static void errore(const char *motivo)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddFalseToObject(r, "ok");
    cJSON_AddStringToObject(r, "motivo", motivo);
    rispondi(r);
}

static bool esa(const char *s, uint8_t *fuori, size_t n)
{
    if (!s || strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1])) return false;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        fuori[i] = (uint8_t)v;
    }
    return true;
}

static const char *testo_di(cJSON *o, const char *nome)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, nome);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void esegui(const char *riga)
{
    cJSON *o = cJSON_Parse(riga);
    if (!o) { errore("json"); return; }
    const char *cmd = testo_di(o, "cmd");

    if (cmd && strcmp(cmd, "info") == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddTrueToObject(r, "ok");
        cJSON_AddStringToObject(r, "versione", esp_app_get_description()->version);
        cJSON_AddBoolToObject(r, "abbinato", memoria_abbinato());
        cJSON *nomi = cJSON_AddArrayToObject(r, "reti");
        rete_t reti[MAX_RETI];
        int n = memoria_reti(reti);
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(nomi, cJSON_CreateString(reti[i].ssid));
        memset(reti, 0, sizeof reti);
        rispondi(r);
    } else if (cmd && strcmp(cmd, "abbina") == 0) {
        const char *ponte = testo_di(o, "ponte"), *gettone = testo_di(o, "gettone"), *chiave_hex = testo_di(o, "chiave");
        cJSON *lp = cJSON_GetObjectItemCaseSensitive(o, "lung_pin");
        int lung_pin = cJSON_IsNumber(lp) ? lp->valueint : LUNG_PIN_MIN;   // pagine del giro 2: 4
        uint8_t chiave[32], g[32];
        if (!ponte || strncmp(ponte, "https://", 8) != 0 || strlen(ponte) > 150) errore("ponte");
        else if (!esa(gettone, g, 32)) errore("gettone");
        else if (!esa(chiave_hex, chiave, 32)) errore("chiave");
        else if (lung_pin < LUNG_PIN_MIN || lung_pin > LUNG_PIN_MAX) errore("lung_pin");
        else if (!memoria_abbina(ponte, gettone, chiave, lung_pin)) errore("memoria");
        else { cJSON *r = cJSON_CreateObject(); cJSON_AddTrueToObject(r, "ok"); rispondi(r); if (avvisa) avvisa(); }
        memset(chiave, 0, sizeof chiave);
        memset(g, 0, sizeof g);
    } else if (cmd && strcmp(cmd, "wifi") == 0) {
        const char *ssid = testo_di(o, "ssid"), *password = testo_di(o, "password");
        if (!ssid || !*ssid || strlen(ssid) > 32) errore("ssid");
        else if (!password || strlen(password) > 64) errore("password");
        else if (!memoria_aggiungi_rete(ssid, password)) errore("reti_piene");
        else { cJSON *r = cJSON_CreateObject(); cJSON_AddTrueToObject(r, "ok"); rispondi(r); rete_ricarica(); if (avvisa) avvisa(); }
    } else if (cmd && strcmp(cmd, "wifi_togli") == 0) {
        const char *ssid = testo_di(o, "ssid");
        if (!ssid || !memoria_togli_rete(ssid)) errore("ssid");
        else { cJSON *r = cJSON_CreateObject(); cJSON_AddTrueToObject(r, "ok"); rispondi(r); rete_ricarica(); if (avvisa) avvisa(); }
    } else if (cmd && strcmp(cmd, "riavvia") == 0) {
        cJSON *r = cJSON_CreateObject(); cJSON_AddTrueToObject(r, "ok"); rispondi(r);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        errore("comando");
    }
    // il testo del comando (con eventuali segreti) non resta in memoria piu' del necessario
    cJSON_Delete(o);
}

static void compito(void *arg)
{
    (void)arg;
    static char riga[MAX_RIGA];
    size_t n = 0;
    bool troppo = false;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            if (!troppo && n > 0) { riga[n] = 0; esegui(riga); }
            else if (troppo) errore("riga_lunga");
            memset(riga, 0, n);
            n = 0;
            troppo = false;
            continue;
        }
        if (n < MAX_RIGA - 1) riga[n++] = (char)c;
        else troppo = true;
    }
}

void seriale_avvia(void (*cambiato)(void))
{
    avvisa = cambiato;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();   // printf passa dal driver, insieme ai log
    xTaskCreate(compito, "seriale", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "pronto per l'abbinamento col cavo");
}

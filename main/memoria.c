#include "memoria.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "memoria";
static const char *SPAZIO = "smartai";

void memoria_avvia(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Memoria lasciata dal firmware di fabbrica in un formato diverso: si ricomincia da zero.
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
}

static bool apri(nvs_open_mode_t modo, nvs_handle_t *h)
{
    return nvs_open(SPAZIO, modo, h) == ESP_OK;
}

static bool leggi_testo(const char *chiave, char *buf, size_t cap)
{
    nvs_handle_t h;
    if (!apri(NVS_READONLY, &h)) return false;
    size_t lung = cap;
    esp_err_t e = nvs_get_str(h, chiave, buf, &lung);
    nvs_close(h);
    return e == ESP_OK;
}

bool memoria_abbinato(void)
{
    uint8_t k[32];
    char g[65];
    return memoria_chiave(k) && memoria_gettone(g, sizeof g);
}

bool memoria_abbina(const char *ponte, const char *gettone_hex, const uint8_t chiave[32], int lung_pin)
{
    if (lung_pin < LUNG_PIN_MIN || lung_pin > LUNG_PIN_MAX) return false;
    nvs_handle_t h;
    if (!apri(NVS_READWRITE, &h)) return false;
    bool ok = nvs_set_str(h, "ponte", ponte) == ESP_OK
        && nvs_set_str(h, "gettone", gettone_hex) == ESP_OK
        && nvs_set_blob(h, "chiave", chiave, 32) == ESP_OK
        && nvs_set_u64(h, "contatore", 0) == ESP_OK
        && nvs_set_u64(h, "visto_pc", 0) == ESP_OK
        && nvs_set_u8(h, "lung_pin", (uint8_t)lung_pin) == ESP_OK
        && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGI(TAG, "abbinamento %s", ok ? "salvato" : "NON salvato");
    return ok;
}

int memoria_lung_pin(void)
{
    nvs_handle_t h;
    uint8_t n = 0;
    if (apri(NVS_READONLY, &h)) { nvs_get_u8(h, "lung_pin", &n); nvs_close(h); }
    return (n >= LUNG_PIN_MIN && n <= LUNG_PIN_MAX) ? n : LUNG_PIN_MIN;
}

bool memoria_ponte(char *url, size_t cap) { return leggi_testo("ponte", url, cap); }
bool memoria_gettone(char *hex, size_t cap) { return leggi_testo("gettone", hex, cap); }

bool memoria_chiave(uint8_t chiave[32])
{
    nvs_handle_t h;
    if (!apri(NVS_READONLY, &h)) return false;
    size_t lung = 32;
    esp_err_t e = nvs_get_blob(h, "chiave", chiave, &lung);
    nvs_close(h);
    return e == ESP_OK && lung == 32;
}

uint64_t memoria_contatore_nuovo(void)
{
    nvs_handle_t h;
    uint64_t c = 0;
    if (!apri(NVS_READWRITE, &h)) return 0;
    nvs_get_u64(h, "contatore", &c);
    c += 1;
    bool ok = nvs_set_u64(h, "contatore", c) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok ? c : 0;   // 0 = non salvato: chi chiama non spedisce
}

uint64_t memoria_visto_pc(void)
{
    nvs_handle_t h;
    uint64_t c = 0;
    if (apri(NVS_READONLY, &h)) { nvs_get_u64(h, "visto_pc", &c); nvs_close(h); }
    return c;
}

void memoria_imposta_visto_pc(uint64_t c)
{
    nvs_handle_t h;
    if (!apri(NVS_READWRITE, &h)) return;
    nvs_set_u64(h, "visto_pc", c);
    nvs_commit(h);
    nvs_close(h);
}

int memoria_reti(rete_t reti[MAX_RETI])
{
    int n = 0;
    for (int i = 0; i < MAX_RETI; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof ks, "ssid%d", i);
        snprintf(kp, sizeof kp, "pass%d", i);
        if (!leggi_testo(ks, reti[n].ssid, sizeof reti[n].ssid)) continue;
        if (!leggi_testo(kp, reti[n].password, sizeof reti[n].password)) reti[n].password[0] = 0;
        n++;
    }
    return n;
}

bool memoria_aggiungi_rete(const char *ssid, const char *password)
{
    int libero = -1;
    for (int i = 0; i < MAX_RETI; i++) {
        char ks[8], s[33];
        snprintf(ks, sizeof ks, "ssid%d", i);
        if (leggi_testo(ks, s, sizeof s)) {
            if (strcmp(s, ssid) == 0) { libero = i; break; }   // stesso nome: si sostituisce
        } else if (libero < 0) {
            libero = i;
        }
    }
    if (libero < 0) return false;
    nvs_handle_t h;
    if (!apri(NVS_READWRITE, &h)) return false;
    char ks[8], kp[8];
    snprintf(ks, sizeof ks, "ssid%d", libero);
    snprintf(kp, sizeof kp, "pass%d", libero);
    bool ok = nvs_set_str(h, ks, ssid) == ESP_OK && nvs_set_str(h, kp, password) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool memoria_ultima_rete(char *ssid, size_t cap) { return leggi_testo("rete_ok", ssid, cap); }

void memoria_imposta_ultima_rete(const char *ssid)
{
    nvs_handle_t h;
    if (!apri(NVS_READWRITE, &h)) return;
    nvs_set_str(h, "rete_ok", ssid);
    nvs_commit(h);
    nvs_close(h);
}

bool memoria_togli_rete(const char *ssid)
{
    nvs_handle_t h;
    if (!apri(NVS_READWRITE, &h)) return false;
    bool tolta = false;
    for (int i = 0; i < MAX_RETI; i++) {
        char ks[8], kp[8], s[33];
        snprintf(ks, sizeof ks, "ssid%d", i);
        snprintf(kp, sizeof kp, "pass%d", i);
        size_t lung = sizeof s;
        if (nvs_get_str(h, ks, s, &lung) == ESP_OK && strcmp(s, ssid) == 0) {
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
            tolta = true;
        }
    }
    nvs_commit(h);
    nvs_close(h);
    return tolta;
}

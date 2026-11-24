// smart-AI, firmware dell'orologio Waveshare ESP32-S3-Touch-AMOLED-2.06.
//
// Giro 2 (24/11/2026): abbinamento col cavo (seriale.c), wifi e ponte (rete.c), PIN sul tastierino.
// Nessun segreto nel codice: wifi, indirizzo del ponte, gettone e chiave delle buste li scrive la
// pagina Orologio dell'app attraverso il cavo, e restano nella memoria dell'orologio (memoria.c).
// Il PIN non resta nell'orologio: va al PC dentro una busta cifrata, il PC lo confronta con l'impronta.
//
// Base: esempio 02_lvgl_demo_v9 e componente VibrationTest del firmware di fabbrica, dal repository
// ufficiale github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (Apache 2.0).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "cJSON.h"
#include "memoria.h"
#include "rete.h"
#include "seriale.h"

static const char *TAG = "smart-ai";

// Motorino: piedino 18, mai acceso piu' di MOTORE_MAX_MS per volta (come VibrationTest di Waveshare).
#define MOTORE        GPIO_NUM_18
#define MOTORE_MAX_MS 250
#define LUNG_PIN      6

static TaskHandle_t compito_motore = NULL;
static QueueHandle_t richieste = NULL;     // messaggi JSON da spedire al PC, uno per volta

static lv_obj_t *etichetta_stato, *etichetta_messaggio, *etichetta_pin, *tastierino, *riquadro_sbloccato;
static char pin[LUNG_PIN + 1];
static int cifre = 0;
static bool sbloccato = false;

typedef struct { char json[96]; } richiesta_t;

// ---- motorino --------------------------------------------------------------------------------------

static void motore(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t ms = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ms, portMAX_DELAY);
        if (ms > MOTORE_MAX_MS) ms = MOTORE_MAX_MS;
        gpio_set_level(MOTORE, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(MOTORE, 0);
    }
}

static void vibra(uint32_t ms) { if (compito_motore) xTaskNotify(compito_motore, ms, eSetValueWithOverwrite); }

static void vibra_due(void) { vibra(90); vTaskDelay(pdMS_TO_TICKS(220)); vibra(90); }

// ---- schermo (le funzioni *_bloccato vanno chiamate con il blocco di LVGL preso) --------------------

static void visibile(lv_obj_t *o, bool si)
{
    if (si) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void mostra_pin_bloccato(void)
{
    char puntini[LUNG_PIN * 2 + 1] = { 0 };
    for (int i = 0; i < LUNG_PIN; i++) strcat(puntini, i < cifre ? "* " : "_ ");
    lv_label_set_text(etichetta_pin, puntini);
}

static void aggiorna_bloccato(void)
{
    const char *r;
    switch (rete_fase()) {
        case RETE_COLLEGATA: r = "wifi ok"; break;
        case RETE_IN_COLLEGAMENTO: r = "wifi..."; break;
        case RETE_NESSUNA_SALVATA: r = "nessun wifi"; break;
        default: r = "wifi spento"; break;
    }
    lv_label_set_text_fmt(etichetta_stato, "%s  %s", r, rete_nome());
    bool abbinato = memoria_abbinato();
    if (!abbinato) {
        lv_label_set_text(etichetta_messaggio, "Da abbinare: collega il cavo e apri\nsmart-AI > Impostazioni > Orologio");
    }
    visibile(tastierino, abbinato && !sbloccato);
    visibile(etichetta_pin, abbinato && !sbloccato);
    visibile(riquadro_sbloccato, abbinato && sbloccato);
}

static void messaggio(const char *testo)
{
    bsp_display_lock(0);
    lv_label_set_text(etichetta_messaggio, testo);
    aggiorna_bloccato();
    bsp_display_unlock();
}

static void cambiato_dal_cavo(void)
{
    bsp_display_lock(0);
    aggiorna_bloccato();
    bsp_display_unlock();
}

static void spedisci(const char *json)
{
    richiesta_t r;
    strncpy(r.json, json, sizeof r.json - 1);
    r.json[sizeof r.json - 1] = 0;
    if (xQueueSend(richieste, &r, 0) != pdTRUE) return;
    memset(r.json, 0, sizeof r.json);
}

// Tastierino: cifre, C per cancellare, OK quando le cifre sono 6. Callback di LVGL: blocco gia' preso.
static void tasto(lv_event_t *e)
{
    lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
    const char *testo = lv_buttonmatrix_get_button_text(t, lv_buttonmatrix_get_selected_button(t));
    if (!testo) return;
    vibra(30);
    if (strcmp(testo, "C") == 0) {
        cifre = 0;
    } else if (strcmp(testo, "OK") == 0) {
        if (cifre != LUNG_PIN) { lv_label_set_text(etichetta_messaggio, "Il PIN e' di 6 cifre"); return; }
        char json[64];
        pin[LUNG_PIN] = 0;
        snprintf(json, sizeof json, "{\"t\":\"sblocca\",\"pin\":\"%s\"}", pin);
        memset(pin, 0, sizeof pin);
        cifre = 0;
        lv_label_set_text(etichetta_messaggio, "Controllo il PIN sul PC...");
        spedisci(json);
        memset(json, 0, sizeof json);
    } else if (cifre < LUNG_PIN) {
        pin[cifre++] = testo[0];
    }
    mostra_pin_bloccato();
}

static void tocco_stato(lv_event_t *e) { (void)e; vibra(30); lv_label_set_text(etichetta_messaggio, "Chiedo al PC..."); spedisci("{\"t\":\"stato\"}"); }
static void tocco_blocca(lv_event_t *e) { (void)e; vibra(30); spedisci("{\"t\":\"blocca\"}"); }

static lv_obj_t *bottone(lv_obj_t *genitore, const char *testo, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(genitore);
    lv_obj_set_size(b, 300, 90);
    lv_obj_set_style_radius(b, 45, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1E4B5E), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *s = lv_label_create(b);
    lv_label_set_text(s, testo);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_24, 0);
    lv_obj_center(s);
    return b;
}

static void disegna(void)
{
    static const char *mappa[] = { "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", "C", "0", "OK", "" };
    lv_obj_t *schermo = lv_screen_active();
    lv_obj_set_style_bg_color(schermo, lv_color_black(), 0);
    lv_obj_set_style_text_color(schermo, lv_color_white(), 0);

    etichetta_stato = lv_label_create(schermo);
    lv_obj_set_style_text_font(etichetta_stato, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(etichetta_stato, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align(etichetta_stato, LV_ALIGN_TOP_MID, 0, 18);

    etichetta_messaggio = lv_label_create(schermo);
    lv_obj_set_width(etichetta_messaggio, 370);
    lv_label_set_long_mode(etichetta_messaggio, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(etichetta_messaggio, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(etichetta_messaggio, &lv_font_montserrat_18, 0);
    lv_label_set_text(etichetta_messaggio, "smart-AI: inserisci il PIN");
    lv_obj_align(etichetta_messaggio, LV_ALIGN_TOP_MID, 0, 46);

    etichetta_pin = lv_label_create(schermo);
    lv_obj_set_style_text_font(etichetta_pin, &lv_font_montserrat_26, 0);
    lv_obj_align(etichetta_pin, LV_ALIGN_TOP_MID, 0, 104);

    tastierino = lv_buttonmatrix_create(schermo);
    lv_buttonmatrix_set_map(tastierino, mappa);
    lv_obj_set_size(tastierino, 380, 330);
    lv_obj_align(tastierino, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(tastierino, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tastierino, 0, 0);
    lv_obj_set_style_text_font(tastierino, &lv_font_montserrat_26, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tastierino, lv_color_hex(0x172B3D), LV_PART_ITEMS);
    lv_obj_set_style_text_color(tastierino, lv_color_white(), LV_PART_ITEMS);
    lv_obj_add_event_cb(tastierino, tasto, LV_EVENT_VALUE_CHANGED, NULL);

    riquadro_sbloccato = lv_obj_create(schermo);
    lv_obj_remove_style_all(riquadro_sbloccato);
    lv_obj_set_size(riquadro_sbloccato, 380, 230);
    lv_obj_align(riquadro_sbloccato, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(riquadro_sbloccato, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(riquadro_sbloccato, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(riquadro_sbloccato, 24, 0);
    bottone(riquadro_sbloccato, "Stato", tocco_stato);
    bottone(riquadro_sbloccato, "Blocca", tocco_blocca);

    mostra_pin_bloccato();
    aggiorna_bloccato();
}

// ---- dialogo col PC ----------------------------------------------------------------------------------

static void gestisci_risposta(const char *inviato, const char *testo)
{
    cJSON *r = cJSON_Parse(testo);
    bool ok = r && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"));
    cJSON *m = r ? cJSON_GetObjectItemCaseSensitive(r, "motivo") : NULL;
    const char *motivo = cJSON_IsString(m) ? m->valuestring : "";
    char riga[96];

    if (strstr(inviato, "\"sblocca\"")) {
        if (ok) { sbloccato = true; messaggio("Sbloccato per un'ora"); vibra_due(); }
        else if (strcmp(motivo, "pin_errato") == 0) {
            cJSON *rim = cJSON_GetObjectItemCaseSensitive(r, "rimasti");
            snprintf(riga, sizeof riga, "PIN sbagliato: restano %d tentativi", cJSON_IsNumber(rim) ? rim->valueint : 0);
            messaggio(riga); vibra(250);
        } else if (strcmp(motivo, "scollegato") == 0) { messaggio("Tre PIN sbagliati: orologio scollegato, va riabbinato col cavo"); vibra(250); }
        else { snprintf(riga, sizeof riga, "Il PC dice: %s", motivo); messaggio(riga); }
    } else if (strstr(inviato, "\"blocca\"")) {
        sbloccato = false; messaggio("Bloccato. Inserisci il PIN"); vibra(60);
    } else {
        if (ok) { messaggio("PC raggiungibile, orologio sbloccato"); vibra(60); }
        else if (strcmp(motivo, "bloccato") == 0) { sbloccato = false; messaggio("L'ora e' passata: inserisci il PIN"); vibra(60); }
        else { snprintf(riga, sizeof riga, "Il PC dice: %s", motivo); messaggio(riga); }
    }
    cJSON_Delete(r);
}

static void dialogo(void *arg)
{
    (void)arg;
    static char risposta[1024];
    richiesta_t r;
    for (;;) {
        xQueueReceive(richieste, &r, portMAX_DELAY);
        ponte_esito_t e = ponte_scambia(r.json, risposta, sizeof risposta);
        if (e == PONTE_OK) gestisci_risposta(r.json, risposta);
        else { messaggio(ponte_esito_testo(e)); vibra(250); }
        memset(&r, 0, sizeof r);
        memset(risposta, 0, sizeof risposta);
    }
}

// Ogni 2 secondi la riga di stato (wifi) si aggiorna da sola.
static void ritocco(void *arg)
{
    (void)arg;
    for (;;) { vTaskDelay(pdMS_TO_TICKS(2000)); cambiato_dal_cavo(); }
}

void app_main(void)
{
    ESP_LOGI(TAG, "avvio, versione %s", esp_app_get_description()->version);

    gpio_config_t uscita = {
        .pin_bit_mask = 1ULL << MOTORE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&uscita));
    gpio_set_level(MOTORE, 0);
    xTaskCreate(motore, "motore", 2048, NULL, 5, &compito_motore);

    memoria_avvia();
    richieste = xQueueCreate(4, sizeof(richiesta_t));

    bsp_display_start();
    bsp_display_lock(0);
    disegna();
    bsp_display_unlock();

    seriale_avvia(cambiato_dal_cavo);
    rete_avvia();
    xTaskCreate(dialogo, "dialogo", 8192, NULL, 5, NULL);
    xTaskCreate(ritocco, "ritocco", 3072, NULL, 2, NULL);

    vibra(200); // segno di vita all'accensione
}

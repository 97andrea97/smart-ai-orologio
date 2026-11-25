// smart-AI, firmware dell'orologio Waveshare ESP32-S3-Touch-AMOLED-2.06.
//
// Giro 2 (24/11/2026): abbinamento col cavo (seriale.c), wifi e ponte (rete.c), PIN sul tastierino.
// Giro 3 (25/11/2026): aggiornamento via wifi (aggiorna.c), batteria, schermo che si spegne, ora; sblocco col gesto.
// Nessun segreto nel codice: wifi, indirizzo del ponte, gettone e chiave delle buste li scrive la
// pagina Orologio dell'app attraverso il cavo, e restano nella memoria dell'orologio (memoria.c).
// Il PIN non resta nell'orologio: va al PC dentro una busta cifrata, il PC lo confronta con l'impronta.
//
// Base: esempio 02_lvgl_demo_v9 e componente VibrationTest del firmware di fabbrica, dal repository
// ufficiale github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (Apache 2.0).

#include <stdio.h>
#include <string.h>
#include <time.h>
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
#include "aggiorna.h"
#include "batteria.h"
#include "esp_system.h"

static const char *TAG = "smart-ai";

// Motorino: piedino 18, mai acceso piu' di MOTORE_MAX_MS per volta (come VibrationTest di Waveshare).
#define MOTORE        GPIO_NUM_18
#define MOTORE_MAX_MS 250

static TaskHandle_t compito_motore = NULL;
static QueueHandle_t richieste = NULL;     // messaggi JSON da spedire al PC, uno per volta

static lv_obj_t *etichetta_ora, *etichetta_stato, *etichetta_messaggio, *riquadro_sbloccato;
// Gesto di sblocco (25/11/2026, al posto del PIN): 9 punti su quasi tutto lo schermo, come lo sblocco di Android. Il gesto
// e' la sequenza dei punti toccati (1-9 per righe, dall'alto a sinistra), ognuno al massimo una volta, almeno GESTO_MIN;
// va al PC nel campo "pin" di "sblocca" e il PC lo confronta con l'impronta, come faceva col PIN.
// Misure in pixel, assolute: dipendono dallo schermo da 410x502. GESTO_RAGGIO largo apposta, perche' sia sensibile.
#define GESTO_LATO   390
#define GESTO_PASSO  130
#define GESTO_RAGGIO 58
#define GESTO_MIN    4
#define COLORE_PUNTO  0x3A4048
#define COLORE_ACCESO 0x7AA9A1
static lv_obj_t *griglia, *punti[9], *traccia;
static lv_point_precise_t tratti[9];
static char gesto[10];
static int n_gesto = 0;
static bool sbloccato = false;
// Batteria: letta ogni LETTURA_BATTERIA_OGNI giri della riga di stato (un giro = 2 s, quindi ogni 30 s): assoluta, la
// percentuale cambia lentamente e ogni lettura e' una chiamata sul bus I2C. -1 = non letta.
#define LETTURA_BATTERIA_OGNI 15
static int batt_percento = -1;
// Schermo: si spegne dopo SCHERMO_SPENTO_DOPO_MS senza tocchi (25/11/2026, batteria). Assoluta: e' il tempo di lettura
// di una riga dello schermo; diventera' una preferenza impostabile dall'app. Lo riaccendono PWR, BOOT, un tocco (che non
// arriva ai tasti) o un messaggio del PC. PWR a schermo acceso lo spegne subito. Controllo ogni VEGLIA_PASSO_MS.
#define SCHERMO_SPENTO_DOPO_MS 30000   // 30 s, scelta del 25/11/2026 (15 s era troppo poco)
#define VEGLIA_PASSO_MS        200
#define TASTO_BOOT             GPIO_NUM_0
static volatile bool schermo_spento = false;
static lv_obj_t *velo = NULL;   // copre tutto a schermo spento: il primo tocco riaccende e basta
static bool batt_in_carica = false;

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

static void aggiorna_bloccato(void)
{
    const char *r;
    switch (rete_fase()) {
        case RETE_COLLEGATA: r = "wifi ok"; break;
        case RETE_IN_COLLEGAMENTO: r = "wifi..."; break;
        case RETE_NESSUNA_SALVATA: r = "nessun wifi"; break;
        default: r = "wifi spento"; break;
    }
    char batt[16] = "--%";
    if (batt_percento >= 0) snprintf(batt, sizeof batt, "%d%%%s", batt_percento, batt_in_carica ? "+" : "");
    lv_label_set_text_fmt(etichetta_stato, "%s   %s  %s", batt, r, rete_nome());
    time_t adesso = time(NULL);
    struct tm t;
    localtime_r(&adesso, &t);
    if (t.tm_year + 1900 >= 2025) lv_label_set_text_fmt(etichetta_ora, "%02d:%02d", t.tm_hour, t.tm_min);
    else lv_label_set_text(etichetta_ora, "--:--");   // ora non ancora presa dal wifi
    bool abbinato = memoria_abbinato();
    if (!abbinato) {
        lv_label_set_text(etichetta_messaggio, "Da abbinare: collega il cavo e apri\nsmart-AI > Impostazioni > Orologio");
    }
    visibile(griglia, abbinato && !sbloccato);
    visibile(riquadro_sbloccato, abbinato && sbloccato);
}

static void accendi_bloccato(void)
{
    if (!schermo_spento) return;
    lv_obj_add_flag(velo, LV_OBJ_FLAG_HIDDEN);
    bsp_display_backlight_on();
    lv_display_trigger_activity(NULL);
    schermo_spento = false;
}

static void spegni_bloccato(void)
{
    if (schermo_spento) return;
    lv_obj_remove_flag(velo, LV_OBJ_FLAG_HIDDEN);
    bsp_display_backlight_off();
    schermo_spento = true;
}

static void tocco_velo(lv_event_t *e) { (void)e; accendi_bloccato(); }   // callback di LVGL: blocco gia' preso

static void messaggio(const char *testo)
{
    bsp_display_lock(0);
    accendi_bloccato();   // una risposta del PC si vede subito
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

static void azzera_gesto(void)
{
    n_gesto = 0;
    memset(gesto, 0, sizeof gesto);
    lv_line_set_points(traccia, tratti, 0);
    for (int i = 0; i < 9; i++) lv_obj_set_style_bg_color(punti[i], lv_color_hex(COLORE_PUNTO), 0);
}

// Callback di LVGL sulla griglia (blocco gia' preso): il dito accende i punti che attraversa; al rilascio, se i punti
// sono abbastanza, il gesto parte per il PC e il disegno si cancella subito (chi guarda non lo vede restare).
static void gesto_evento(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) azzera_gesto();
    if (c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING) {
        lv_indev_t *in = lv_indev_active();
        if (!in) return;
        lv_point_t p;
        lv_area_t a;
        lv_indev_get_point(in, &p);
        lv_obj_get_coords(griglia, &a);
        int x = p.x - a.x1, y = p.y - a.y1;
        for (int i = 0; i < 9 && n_gesto < 9; i++) {
            int cx = GESTO_PASSO / 2 + (i % 3) * GESTO_PASSO, cy = GESTO_PASSO / 2 + (i / 3) * GESTO_PASSO;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > GESTO_RAGGIO * GESTO_RAGGIO) continue;
            if (memchr(gesto, '1' + i, n_gesto)) continue;
            gesto[n_gesto] = (char)('1' + i);
            tratti[n_gesto].x = cx;
            tratti[n_gesto].y = cy;
            n_gesto++;
            lv_line_set_points(traccia, tratti, n_gesto);
            lv_obj_set_style_bg_color(punti[i], lv_color_hex(COLORE_ACCESO), 0);
            vibra(15);
        }
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (n_gesto >= GESTO_MIN) {
            char json[64];
            snprintf(json, sizeof json, "{\"t\":\"sblocca\",\"pin\":\"%s\"}", gesto);
            lv_label_set_text(etichetta_messaggio, "Controllo...");
            spedisci(json);
            memset(json, 0, sizeof json);
        } else if (n_gesto > 0) {
            lv_label_set_text(etichetta_messaggio, "Almeno 4 punti");
        }
        azzera_gesto();
    }
}
static void tocco_stato(lv_event_t *e) { (void)e; vibra(30); lv_label_set_text(etichetta_messaggio, "Chiedo al PC..."); spedisci("{\"t\":\"stato\"}"); }
static void tocco_blocca(lv_event_t *e) { (void)e; vibra(30); spedisci("{\"t\":\"blocca\"}"); }

// Chiede al PC se c'e' un programma nuovo; il PC risponde con indirizzo, lunghezza e impronta.
static void tocco_aggiorna(lv_event_t *e)
{
    (void)e;
    vibra(30);
    lv_label_set_text(etichetta_messaggio, "Chiedo al PC se c'e' un aggiornamento...");
    char json[96];
    snprintf(json, sizeof json, "{\"t\":\"aggiorna\",\"versione\":\"%.40s\"}", esp_app_get_description()->version);
    spedisci(json);
}

static lv_obj_t *bottone(lv_obj_t *genitore, const char *testo, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(genitore);
    lv_obj_set_size(b, 300, 80);
    lv_obj_set_style_radius(b, 40, 0);
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
    lv_obj_t *schermo = lv_screen_active();
    lv_obj_set_style_bg_color(schermo, lv_color_black(), 0);
    lv_obj_set_style_text_color(schermo, lv_color_white(), 0);

    etichetta_stato = lv_label_create(schermo);
    lv_obj_set_style_text_font(etichetta_stato, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(etichetta_stato, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align(etichetta_stato, LV_ALIGN_TOP_MID, 0, 10);

    etichetta_ora = lv_label_create(schermo);
    lv_obj_set_style_text_font(etichetta_ora, &lv_font_montserrat_48, 0);
    lv_label_set_text(etichetta_ora, "--:--");
    lv_obj_align(etichetta_ora, LV_ALIGN_TOP_MID, 0, 32);

    etichetta_messaggio = lv_label_create(schermo);
    lv_obj_set_width(etichetta_messaggio, 370);
    lv_label_set_long_mode(etichetta_messaggio, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(etichetta_messaggio, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(etichetta_messaggio, &lv_font_montserrat_18, 0);
    lv_label_set_text(etichetta_messaggio, "Disegna il gesto");
    lv_obj_align(etichetta_messaggio, LV_ALIGN_TOP_MID, 0, 96);

    griglia = lv_obj_create(schermo);
    lv_obj_remove_style_all(griglia);
    lv_obj_set_size(griglia, GESTO_LATO, GESTO_LATO);
    lv_obj_align(griglia, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(griglia, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(griglia, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(griglia, gesto_evento, LV_EVENT_ALL, NULL);
    traccia = lv_line_create(griglia);
    lv_obj_set_size(traccia, GESTO_LATO, GESTO_LATO);
    lv_obj_set_pos(traccia, 0, 0);
    lv_obj_set_style_line_width(traccia, 10, 0);
    lv_obj_set_style_line_color(traccia, lv_color_hex(COLORE_ACCESO), 0);
    lv_obj_set_style_line_rounded(traccia, true, 0);
    lv_obj_remove_flag(traccia, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 9; i++) {
        punti[i] = lv_obj_create(griglia);
        lv_obj_remove_style_all(punti[i]);
        lv_obj_set_size(punti[i], 40, 40);
        lv_obj_set_style_radius(punti[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(punti[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(punti[i], lv_color_hex(COLORE_PUNTO), 0);
        lv_obj_remove_flag(punti[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(punti[i], GESTO_PASSO / 2 + (i % 3) * GESTO_PASSO - 20, GESTO_PASSO / 2 + (i / 3) * GESTO_PASSO - 20);
    }
    riquadro_sbloccato = lv_obj_create(schermo);
    lv_obj_remove_style_all(riquadro_sbloccato);
    lv_obj_set_size(riquadro_sbloccato, 380, 300);
    lv_obj_align(riquadro_sbloccato, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(riquadro_sbloccato, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(riquadro_sbloccato, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(riquadro_sbloccato, 20, 0);
    bottone(riquadro_sbloccato, "Stato", tocco_stato);
    bottone(riquadro_sbloccato, "Blocca", tocco_blocca);
    bottone(riquadro_sbloccato, "Aggiorna", tocco_aggiorna);

    // Versione in piccolo in fondo: dopo un aggiornamento via wifi si vede subito se e' arrivato.
    lv_obj_t *versione = lv_label_create(schermo);
    lv_obj_set_style_text_font(versione, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(versione, lv_color_hex(0x5C6670), 0);
    lv_label_set_text_fmt(versione, "v %s", esp_app_get_description()->version);
    lv_obj_align(versione, LV_ALIGN_BOTTOM_MID, 0, -2);

    aggiorna_bloccato();

    velo = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(velo);
    lv_obj_set_size(velo, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(velo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(velo, tocco_velo, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(velo, LV_OBJ_FLAG_HIDDEN);
}

// ---- dialogo col PC ----------------------------------------------------------------------------------

static void avanzamento(int percento)
{
    char riga[48];
    snprintf(riga, sizeof riga, "Scarico l'aggiornamento: %d%%", percento);
    messaggio(riga);
}

// Risposta del PC a "aggiorna": gia' aperta e autenticata (busta del PC). Si scarica nel posto
// libero; se l'impronta coincide si riavvia sul programma nuovo, che parte "in prova".
static void aggiorna_da(cJSON *r, bool ok, const char *motivo)
{
    char riga[96];
    if (!ok) {
        if (strcmp(motivo, "gia_aggiornato") == 0) messaggio("Nessun aggiornamento: e' gia' l'ultima versione");
        else if (strcmp(motivo, "bloccato") == 0) { sbloccato = false; messaggio("L'ora e' passata: disegna il gesto"); }
        else { snprintf(riga, sizeof riga, "Aggiornamento: il PC dice %s", motivo); messaggio(riga); }
        vibra(60);
        return;
    }
    cJSON *u = cJSON_GetObjectItemCaseSensitive(r, "url");
    cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "dim");
    cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "sha256");
    if (!cJSON_IsString(u) || !cJSON_IsNumber(d) || d->valuedouble < 1 || !cJSON_IsString(s)) {
        messaggio("Aggiornamento: risposta del PC non valida"); vibra(250); return;
    }
    messaggio("Scarico l'aggiornamento...");
    aggiorna_esito_t e = aggiorna_scarica(u->valuestring, (size_t)d->valuedouble, s->valuestring, avanzamento);
    if (e != AGGIORNA_OK) { messaggio(aggiorna_esito_testo(e)); vibra(250); return; }
    messaggio("Aggiornato e verificato: riavvio");
    vibra_due();
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

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
            snprintf(riga, sizeof riga, "Gesto sbagliato: restano %d tentativi", cJSON_IsNumber(rim) ? rim->valueint : 0);
            messaggio(riga); vibra(250);
        } else if (strcmp(motivo, "scollegato") == 0) { messaggio("Tre gesti sbagliati: orologio scollegato, va riabbinato col cavo"); vibra(250); }
        else { snprintf(riga, sizeof riga, "Il PC dice: %s", motivo); messaggio(riga); }
    } else if (strstr(inviato, "\"aggiorna\"")) {
        aggiorna_da(r, ok, motivo);
    } else if (strstr(inviato, "\"blocca\"")) {
        sbloccato = false; messaggio("Bloccato"); vibra(60);
    } else {
        if (ok) { messaggio("PC raggiungibile, orologio sbloccato"); vibra(60); }
        else if (strcmp(motivo, "bloccato") == 0) { sbloccato = false; messaggio("L'ora e' passata: disegna il gesto"); vibra(60); }
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
        if (e == PONTE_OK) {
            aggiorna_conferma();   // il PC ha risposto: il programma nuovo funziona e resta
            gestisci_risposta(r.json, risposta);
        }
        else { messaggio(ponte_esito_testo(e)); vibra(250); }
        memset(&r, 0, sizeof r);
        memset(risposta, 0, sizeof risposta);
    }
}

// Veglia: tasti PWR e BOOT, e spegnimento dello schermo dopo un po' senza tocchi.
static void veglia(void *arg)
{
    (void)arg;
    bool boot_prima = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(VEGLIA_PASSO_MS));
        bool pwr = batteria_tasto_pwr();
        bool boot = gpio_get_level(TASTO_BOOT) == 0;
        bool boot_premuto = boot && !boot_prima;
        boot_prima = boot;
        bsp_display_lock(0);
        if (pwr && !schermo_spento) spegni_bloccato();
        else if ((pwr || boot_premuto) && schermo_spento) { accendi_bloccato(); vibra(30); }
        else if (!schermo_spento && lv_display_get_inactive_time(NULL) > SCHERMO_SPENTO_DOPO_MS) spegni_bloccato();
        bsp_display_unlock();
    }
}

// Ogni 2 secondi la riga di stato (wifi) si aggiorna da sola.
static void ritocco(void *arg)
{
    (void)arg;
    for (int giro = 0;; giro++) {
        if (giro % LETTURA_BATTERIA_OGNI == 0) {
            int p; bool c;
            if (batteria_leggi(&p, &c)) { batt_percento = p; batt_in_carica = c; }
        }
        cambiato_dal_cavo();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
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

    // Schermo PRIMA del wifi: la sua memoria per i trasferimenti si prende quando c'e'. Il pacchetto della
    // scheda e' la copia corretta di Waveshare in components\ (vedi components\LEGGIMI.md).
    bsp_display_start();
    bsp_display_lock(0);
    disegna();
    // La copia di fabbrica parte con luminosita' 0 (bsp/display.h: "The display's backlight is not turned on"):
    // senza questa riga lo schermo resta nero (visto sul vero il 25/11/2026). Come il firmware di fabbrica
    // (show_startup_screen in examples/factory/main/main.cpp): dopo la prima schermata e col blocco preso.
    bsp_display_backlight_on();
    bsp_display_unlock();
    batteria_avvia();   // dopo lo schermo: il bus I2C e' gia' preparato dal pacchetto della scheda (touch)

    seriale_avvia(cambiato_dal_cavo);
    rete_avvia();
    xTaskCreate(dialogo, "dialogo", 8192, NULL, 5, NULL);
    xTaskCreate(ritocco, "ritocco", 3072, NULL, 2, NULL);

    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << TASTO_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);
    xTaskCreate(veglia, "veglia", 3072, NULL, 3, NULL);

    vibra(200); // segno di vita all'accensione

    // Programma appena arrivato via wifi: si prova subito a parlare col PC. Se risponde resta questo,
    // altrimenti al prossimo riavvio (PWR) torna il precedente.
    if (aggiorna_in_prova()) {
        messaggio("Programma nuovo: controllo che parli col PC...");
        spedisci("{\"t\":\"stato\"}");
    }
}

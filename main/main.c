// smart-AI, firmware dell'orologio Waveshare ESP32-S3-Touch-AMOLED-2.06.
//
// Giro 1 (24/11/2026): prova la catena intera (compilazione su GitHub Actions, caricamento con
// esptool-js, schermo, tocco, motorino) prima di metterci wifi, ponte e PIN. Nessun segreto qui
// dentro: wifi, gettone, chiave delle buste e PIN si inseriscono sul dispositivo, non stanno nel codice.
//
// Base: esempio 02_lvgl_demo_v9 e componente VibrationTest del firmware di fabbrica, dal repository
// ufficiale github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 (Apache 2.0).

#include <stdio.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "smart-ai";

// Motorino di vibrazione: piedino 18, come in VibrationTest.hpp di Waveshare. Mai acceso piu' di
// MOTORE_MAX_MS per volta (lo stesso limite del firmware di fabbrica).
#define MOTORE        GPIO_NUM_18
#define MOTORE_MAX_MS 250

static TaskHandle_t compito_motore = NULL;
static lv_obj_t *etichetta_tocchi = NULL;
static unsigned tocchi = 0;

// Il motorino gira in un compito suo, cosi' lo schermo non si ferma mentre vibra.
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

static void vibra(uint32_t ms)
{
    if (compito_motore) xTaskNotify(compito_motore, ms, eSetValueWithOverwrite);
}

static void tocco_vibra(lv_event_t *e)
{
    (void)e;
    tocchi += 1;
    lv_label_set_text_fmt(etichetta_tocchi, "Tocchi: %u", tocchi);
    vibra(150);
}

static void disegna(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *schermo = lv_screen_active();
    lv_obj_set_style_bg_color(schermo, lv_color_black(), 0);

    lv_obj_t *titolo = lv_label_create(schermo);
    lv_label_set_text(titolo, "smart-AI");
    lv_obj_set_style_text_font(titolo, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(titolo, lv_color_white(), 0);
    lv_obj_align(titolo, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *versione = lv_label_create(schermo);
    lv_label_set_text_fmt(versione, "giro 1 - %s", app->version);
    lv_obj_set_style_text_font(versione, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(versione, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align(versione, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *tasto = lv_button_create(schermo);
    lv_obj_set_size(tasto, 200, 200);
    lv_obj_set_style_radius(tasto, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tasto, lv_color_hex(0x1E4B5E), 0);
    lv_obj_align(tasto, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_event_cb(tasto, tocco_vibra, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scritta = lv_label_create(tasto);
    lv_label_set_text(scritta, "Vibra");
    lv_obj_set_style_text_font(scritta, &lv_font_montserrat_24, 0);
    lv_obj_center(scritta);

    etichetta_tocchi = lv_label_create(schermo);
    lv_label_set_text(etichetta_tocchi, "Tocchi: 0");
    lv_obj_set_style_text_font(etichetta_tocchi, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(etichetta_tocchi, lv_color_white(), 0);
    lv_obj_align(etichetta_tocchi, LV_ALIGN_BOTTOM_MID, 0, -40);
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

    bsp_display_start();
    bsp_display_lock(0);
    disegna();
    bsp_display_unlock();

    vibra(200); // segno di vita all'accensione
}

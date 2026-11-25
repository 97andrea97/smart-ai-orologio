// Batteria (25/11/2026): percentuale e carica dal chip di gestione AXP2101, indirizzo 0x34 sullo stesso
// bus I2C del touch (BSP_I2C_SCL 14, BSP_I2C_SDA 15), preparato dal pacchetto della scheda.
// Registri presi da XPowersLib, la libreria che Waveshare usa nell'esempio ufficiale
// examples/esp-idf/01_AXP2101 (src/REG/AXP2101Constants.h e XPowersAXP2101.tpp):
//   0x00 STATUS1 bit 3 = batteria collegata
//   0x01 STATUS2 bit 7..5 = 001 in carica
//   0x18 bit 3 = misuratore di carica (gauge) acceso
//   0x30 bit 0 = misura della tensione di batteria accesa
//   0x68 bit 0 = rilevamento della batteria acceso
//   0xA4 = percentuale (0-100)
//   0x34 bit 4..0 e 0x35 = tensione della batteria in millivolt (ADC_DATA_RELUST0/1; getBattVoltage legge i due
//        registri con readRegisterH5L8: ((alto & 0x1F) << 8) | basso), con la misura accesa dal bit 0 di 0x30
//   0x41 bit 3 = abilita l'avviso di pressione breve del tasto PWR (PKEY_SHORT_IRQ = bit 11, secondo registro)
//   0x49 bit 3 = pressione breve avvenuta; si azzera scrivendo 1 su quel bit
#include "batteria.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "batteria";
static i2c_master_dev_handle_t pmu = NULL;

#define INDIRIZZO   0x34
#define ATTESA_MS   100

static bool leggi(uint8_t reg, uint8_t *val)
{
    return pmu && i2c_master_transmit_receive(pmu, &reg, 1, val, 1, ATTESA_MS) == ESP_OK;
}

static bool accendi_bit(uint8_t reg, int bit)
{
    uint8_t v;
    if (!leggi(reg, &v)) return false;
    if (v & (1u << bit)) return true;
    uint8_t buf[2] = { reg, (uint8_t)(v | (1u << bit)) };
    return i2c_master_transmit(pmu, buf, 2, ATTESA_MS) == ESP_OK;
}

void batteria_avvia(void)
{
    if (bsp_i2c_init() != ESP_OK) { ESP_LOGW(TAG, "bus I2C non pronto"); return; }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INDIRIZZO,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &pmu) != ESP_OK) { pmu = NULL; ESP_LOGW(TAG, "AXP2101 non aggiunto"); return; }
    bool ok = accendi_bit(0x68, 0) && accendi_bit(0x30, 0) && accendi_bit(0x18, 3) && accendi_bit(0x41, 3);
    uint8_t azzera[2] = { 0x49, 0x08 };
    if (ok) i2c_master_transmit(pmu, azzera, 2, ATTESA_MS);
    ESP_LOGI(TAG, "AXP2101 %s", ok ? "pronto" : "non risponde");
}

bool batteria_leggi(int *percento, bool *in_carica)
{
    uint8_t s1, s2, p;
    if (!leggi(0x00, &s1) || !(s1 & (1u << 3))) return false;   // niente batteria o niente risposta
    if (!leggi(0xA4, &p) || !leggi(0x01, &s2)) return false;
    *percento = p > 100 ? 100 : p;
    *in_carica = (s2 >> 5) == 0x01;
    return true;
}

// Come getBattVoltage di XPowersLib (src/XPowersAXP2101.hpp): 0 se la batteria non c'e', qui false.
bool batteria_tensione(int *mv)
{
    uint8_t s1, alto, basso;
    if (!leggi(0x00, &s1) || !(s1 & (1u << 3))) return false;
    if (!leggi(0x34, &alto) || !leggi(0x35, &basso)) return false;
    *mv = ((alto & 0x1F) << 8) | basso;
    return true;
}

bool batteria_tasto_pwr(void)
{
    uint8_t s;
    if (!leggi(0x49, &s) || !(s & 0x08)) return false;
    uint8_t azzera[2] = { 0x49, 0x08 };
    i2c_master_transmit(pmu, azzera, 2, ATTESA_MS);
    return true;
}

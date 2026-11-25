// Memoria permanente dell'orologio (NVS, spazio "smartai"). Qui stanno, dopo l'abbinamento col cavo:
// indirizzo del ponte, gettone, chiave delle buste, contatori e reti wifi. Niente di questo e' nel
// codice: lo scrive la pagina Orologio dell'app attraverso il cavo (seriale.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_RETI 3

typedef struct {
    char ssid[33];
    char password[65];
} rete_t;

void memoria_avvia(void);

bool memoria_abbinato(void);
// Abbinamento: sostituisce ponte, gettone e chiave, azzera i contatori.
// lung_pin: quante cifre ha il PIN (lo decide il PC, giro 3); fuori da LUNG_PIN_MIN..LUNG_PIN_MAX si rifiuta.
bool memoria_abbina(const char *ponte, const char *gettone_hex, const uint8_t chiave[32], int lung_pin);
#define LUNG_PIN_MIN 4
#define LUNG_PIN_MAX 8
int memoria_lung_pin(void);   // LUNG_PIN_MIN se non salvata
bool memoria_ponte(char *url, size_t cap);
bool memoria_gettone(char *hex, size_t cap);
bool memoria_chiave(uint8_t chiave[32]);

// Contatore delle buste dell'orologio: cresce e si salva PRIMA di spedire (PROTOCOLLO.md).
uint64_t memoria_contatore_nuovo(void);
uint64_t memoria_visto_pc(void);
void memoria_imposta_visto_pc(uint64_t c);

int memoria_reti(rete_t reti[MAX_RETI]);
// Aggiunge o sostituisce (stesso nome); false se sono gia' MAX_RETI e il nome e' nuovo.
bool memoria_aggiungi_rete(const char *ssid, const char *password);
bool memoria_togli_rete(const char *ssid);

// Wifi e ponte. Le buste sono quelle di PROTOCOLLO.md (nonce 12 | cifrato | sigillo 16, AES-256-GCM),
// le stesse che il PC apre e sigilla in pc/protocollo.mjs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { RETE_SPENTA, RETE_NESSUNA_SALVATA, RETE_IN_COLLEGAMENTO, RETE_COLLEGATA } rete_fase_t;

void rete_avvia(void);

// Misure della rete dall'avvio (26/11/2026), per la telemetria verso il PC (telemetria.c): richieste fatte al ponte, connessioni
// sicure aperte da zero con il tempo che e' costata la loro apertura (DNS, TCP, stretta di mano TLS), e quando e' arrivato
// il primo indirizzo dalla wifi (ms dall'avvio, 0 = mai). Chi misura un giro ne fa la differenza fra due letture.
typedef struct { uint32_t richieste, collegamenti, ms_apertura, ms_wifi; } rete_statistiche_t;
void rete_statistiche(rete_statistiche_t *s);
// Forza del segnale della wifi a cui e' collegato, in dBm (negativo), 0 se non e' collegato. Mai il nome della rete.
int rete_segnale(void);
// Rilegge le reti salvate e ricomincia a collegarsi (dopo che la pagina ne ha aggiunta una).
void rete_ricarica(void);
// Schermata Wifi dell'orologio (27/11/2026, wifi_schermo.c). rete_pausa(true) ferma il giro fra le reti salvate (la
// scansione fallisce mentre ne prova una), rete_pausa(false) lo fa ripartire. rete_ricarica_prima rilegge le reti salvate
// e prova per prima `ssid`; rete_esito_nuova: -1 in corso, 0 collegata, altrimenti il motivo del rifiuto
// (wifi_err_reason_t: 15 password sbagliata, 201 rete non trovata). Mai password qui.
void rete_pausa(bool si);
void rete_ricarica_prima(const char *ssid);
int rete_esito_nuova(void);
typedef struct { char ssid[33]; int8_t rssi; bool aperta; } rete_vista_t;
bool rete_scansiona(void);                  // false se il driver non la fa partire (si riprova)
bool rete_scansione_finita(bool *ok);
int rete_scansione_leggi(rete_vista_t *fuori, int max);   // ordinate per segnale; libera la lista del driver
void rete_scansione_ferma(void);
rete_fase_t rete_fase(void);
const char *rete_nome(void);   // la rete a cui e' collegato o che sta provando, "" se nessuna
bool rete_attendi(int ms);     // true se collegata entro ms

typedef enum {
    PONTE_OK = 0,
    PONTE_NON_ABBINATO,
    PONTE_SENZA_RETE,
    PONTE_PC_SPENTO,        // 503: il PC non e' collegato al ponte
    PONTE_RIFIUTATO,        // 401: l'orologio non e' (piu') riconosciuto
    PONTE_NESSUNA_RISPOSTA, // il PC non ha risposto in tempo
    PONTE_ERRORE,
} ponte_esito_t;

// Spedisce il messaggio JSON al PC e mette la risposta JSON (gia' aperta e controllata) in `risposta`.
ponte_esito_t ponte_scambia(const char *json, char *risposta, size_t cap);
// Come ponte_scambia, per un messaggio di `lung` byte anche grande (i pezzi della voce, centinaia di KB): busta e
// risposta stanno nella PSRAM. Con attendi=false non ritira la risposta (i pezzi intermedi della voce: il PC
// risponde lo stesso e il ponte butta la risposta dopo dieci minuti); `risposta` resta com'era.
ponte_esito_t ponte_manda(const char *json, size_t lung, bool attendi, char *risposta, size_t cap);
const char *ponte_esito_testo(ponte_esito_t e);

// La connessione sicura al ponte resta aperta fra una richiesta e l'altra (26/11/2026, rete.c): ponte_chiudi la chiude
// (prima di registrare, di suonare, di aggiornare: servono memoria e tempo; aspetta fino a 2 s uno scambio in corso),
// ponte_pulisci la chiude solo se ferma da troppo e senza aspettare (si chiama a ogni giro dal compito che la usa).
void ponte_chiudi(void);
void ponte_pulisci(void);

// Wifi senza risparmio energetico (true) durante un giro di voce, poi di nuovo col risparmio di default (false).
void rete_veloce(bool si);

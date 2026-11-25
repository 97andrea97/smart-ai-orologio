// Aggiornamento via wifi (giro 3, 25/11/2026). Il PC dice, dentro una busta cifrata, da dove
// scaricare il programma nuovo, quanto e' lungo e la sua sha256. L'orologio lo scrive nel posto
// libero calcolando l'impronta mentre scarica, e si avvia da li' solo se impronta e lunghezza
// coincidono. Da dove arriva il file non conta: conta solo l'impronta mandata dal PC.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AGGIORNA_OK = 0,        // scritto e verificato: chi chiama riavvia
    AGGIORNA_DATI,          // url, lunghezza o impronta non validi
    AGGIORNA_RETE,          // scaricamento non riuscito
    AGGIORNA_IMPRONTA,      // il file scaricato non e' quello detto dal PC: scartato
    AGGIORNA_MEMORIA,       // errore nello scrivere il posto libero
} aggiorna_esito_t;

// `avanzamento` (puo' essere NULL) riceve la percentuale ogni tanto.
aggiorna_esito_t aggiorna_scarica(const char *url, size_t lunghezza, const char *sha256_hex,
                                  void (*avanzamento)(int percento));
const char *aggiorna_esito_testo(aggiorna_esito_t e);

// true se il programma in esecuzione e' appena arrivato e aspetta di dichiararsi buono.
bool aggiorna_in_prova(void);
// Dichiara buono il programma in esecuzione (dopo la prima risposta del PC). Senza, al riavvio
// successivo l'orologio torna al programma precedente.
void aggiorna_conferma(void);

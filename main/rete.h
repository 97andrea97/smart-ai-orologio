// Wifi e ponte. Le buste sono quelle di PROTOCOLLO.md (nonce 12 | cifrato | sigillo 16, AES-256-GCM),
// le stesse che il PC apre e sigilla in pc/protocollo.mjs.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { RETE_SPENTA, RETE_NESSUNA_SALVATA, RETE_IN_COLLEGAMENTO, RETE_COLLEGATA } rete_fase_t;

void rete_avvia(void);
// Rilegge le reti salvate e ricomincia a collegarsi (dopo che la pagina ne ha aggiunta una).
void rete_ricarica(void);
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
const char *ponte_esito_testo(ponte_esito_t e);

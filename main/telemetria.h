// Telemetria dello schermo verso il PC (26/11/2026). Chi guarda l'orologio non deve riferire a voce cio' che c'e' scritto sullo
// schermo: i tempi misurati dall'orologio e ogni errore mostrato vanno da soli al PC, che li scrive nel suo registro
// (PROTOCOLLO.md del PC, "Schermo dell'orologio"). Mai il contenuto dei messaggi, mai il nome della rete ne' la password:
// solo tempi, codici, la versione del programma e la forza del segnale.
//
// Gli eventi stanno in una coda piccola in memoria (pochi elementi: i piu' vecchi si perdono). Li spedisce telemetria_manda(),
// con una richiesta piccola al ponte (di solito sulla connessione sicura gia' aperta); se l'invio fallisce restano in coda per
// la prossima occasione. Un errore della telemetria non si mostra mai sullo schermo e non genera altri eventi.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Come e' finita la parte di voce di un giro (campo `esito` dell'evento tempi).
enum {
    TELEMETRIA_VOCE_SUONATA = 0,
    TELEMETRIA_VOCE_INTERROTTA = 1,
    TELEMETRIA_SENZA_VOCE = 2,       // risposta a testo: il PC non l'ha letta o Silenzioso e' acceso
    TELEMETRIA_VOCE_ERRORE = 3,      // la voce non e' stata scaricata o non ha suonato (c'e' anche un evento errore)
    TELEMETRIA_SENZA_RISPOSTA = 4,   // il giro e' finito con una nota (errore, annullato, fermato) e non con una risposta
};

// I tempi di un giro di voce, in millisecondi, come li ha misurati l'orologio, piu' cosa e' costata la rete in quel giro.
typedef struct {
    uint32_t invio;          // dall'inizio dell'invio dell'audio alla fine
    uint32_t attesa;         // dalla fine dell'invio alla risposta
    uint32_t scarico;        // scaricamento della voce (0 se non scaricata)
    uint32_t riproduzione;   // durata dell'ascolto dall'altoparlante (0 se non ha suonato)
    int esito;               // TELEMETRIA_*
    uint32_t richieste;      // richieste fatte al ponte nel giro
    uint32_t connessioni;    // connessioni sicure aperte da zero nel giro
    uint32_t apertura;       // millisecondi spesi ad aprirle (DNS, TCP, stretta di mano TLS)
    uint32_t su, giu;        // banda misurata in salita e in discesa, byte al secondo (0 se non misurata; 27/11/2026)
    uint32_t cod;            // audio leggero: bit 0 in salita, bit 1 in discesa (0 = PCM)
} telemetria_tempi_t;

// Prepara la coda (nella PSRAM). Da chiamare una volta, prima dei compiti che la usano. Senza memoria la telemetria resta spenta.
void telemetria_avvia(void);
// Un errore mostrato sullo schermo. `origine`: parola corta per dire dove (voce, stato, audio, sblocca, aggiorna, pc...).
// `testo` va via com'e' (accorciato a 159 caratteri, a capo diventano '/'): niente che contenga parole di chi usa l'orologio.
void telemetria_errore(const char *origine, const char *testo);
// I tempi di un giro di voce finito.
void telemetria_tempi(const telemetria_tempi_t *t);
// C'e' qualcosa da spedire (compreso il rapporto di avvio, appena la wifi ha dato un indirizzo)?
bool telemetria_da_mandare(void);
// Spedisce quanto e' in coda, una richiesta sola. true se e' partito (e la coda si e' svuotata di quegli eventi) oppure non c'era
// niente; false se l'invio e' fallito. Blocca fino alla fine della richiesta: va chiamata da un compito che puo' aspettare,
// e non mentre l'orologio registra o suona.
bool telemetria_manda(void);

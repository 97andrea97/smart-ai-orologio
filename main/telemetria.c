#include "telemetria.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "rete.h"

static const char *TAG = "telemetria";

// Pochi elementi in memoria: se il PC non si fa sentire per molto, i piu' vecchi si perdono (restano i 8 piu' recenti).
// Assoluti: bastano a coprire un paio di giri di voce con i loro errori.
#define MAX_EVENTI 8
#define MAX_TESTO  160
// Un messaggio al PC sta qui dentro (nella PSRAM): 8 errori da 160 caratteri tutti con virgolette (che raddoppiano) ci stanno quasi;
// quel che non entra parte con la richiesta dopo.
#define CAP_JSON   4096

typedef struct {
    uint32_t seq;            // numero d'ordine, 0 = posto libero
    uint32_t u;              // ms dall'avvio dell'orologio quando e' nato
    char tipo;               // 'e' errore, 't' tempi, 'a' avvio
    char origine[12];
    char testo[MAX_TESTO];
    uint32_t n[8];           // tempi: invio, attesa, scarico, riproduzione, esito, richieste, connessioni, apertura; avvio: wifi, rst, mem, blk
} evento_t;

static evento_t *eventi = NULL;
static SemaphoreHandle_t blocco = NULL;
static uint32_t prossimo_seq = 1;
static bool avvio_fatto = false;   // solo dal compito che spedisce

static uint32_t ora_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

void telemetria_avvia(void)
{
    if (eventi) return;
    blocco = xSemaphoreCreateMutex();
    evento_t *e = heap_caps_calloc(MAX_EVENTI, sizeof(evento_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blocco || !e) { ESP_LOGW(TAG, "telemetria spenta: memoria"); return; }
    eventi = e;
}

// Solo ASCII stampabile: a capo -> '/', il resto -> '?'. Cosi' il JSON ha da escludere solo virgolette e barre.
static void copia_testo(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (; *src && k + 1 < cap; src++) {
        const unsigned char c = (unsigned char)*src;
        dst[k++] = c == '\n' ? '/' : (c < 0x20 || c > 0x7e) ? '?' : (char)c;
    }
    dst[k] = 0;
}

// Con `blocco` preso: un posto libero, oppure quello dell'evento piu' vecchio.
static evento_t *nuovo(char tipo)
{
    evento_t *p = NULL;
    for (int i = 0; i < MAX_EVENTI; i++) {
        if (eventi[i].seq == 0) { p = &eventi[i]; break; }
        if (!p || eventi[i].seq < p->seq) p = &eventi[i];
    }
    memset(p, 0, sizeof *p);
    p->seq = prossimo_seq++;
    p->u = ora_ms();
    p->tipo = tipo;
    return p;
}

void telemetria_errore(const char *origine, const char *testo)
{
    if (!eventi || !testo) return;
    char o[12] = { 0 }, t[MAX_TESTO] = { 0 };
    copia_testo(o, sizeof o, origine ? origine : "?");
    copia_testo(t, sizeof t, testo);
    xSemaphoreTake(blocco, portMAX_DELAY);
    const evento_t *ultimo = NULL;
    for (int i = 0; i < MAX_EVENTI; i++) if (eventi[i].seq && (!ultimo || eventi[i].seq > ultimo->seq)) ultimo = &eventi[i];
    // Lo stesso errore due volte di fila a pochi secondi di distanza e' uno solo.
    const bool uguale = ultimo && ultimo->tipo == 'e' && strcmp(ultimo->origine, o) == 0 && strcmp(ultimo->testo, t) == 0
                        && (uint32_t)(ora_ms() - ultimo->u) < 3000;
    if (!uguale) {
        evento_t *p = nuovo('e');
        memcpy(p->origine, o, sizeof p->origine);
        memcpy(p->testo, t, sizeof p->testo);
    }
    xSemaphoreGive(blocco);
}

void telemetria_tempi(const telemetria_tempi_t *t)
{
    if (!eventi || !t) return;
    xSemaphoreTake(blocco, portMAX_DELAY);
    evento_t *p = nuovo('t');
    p->n[0] = t->invio;
    p->n[1] = t->attesa;
    p->n[2] = t->scarico;
    p->n[3] = t->riproduzione;
    p->n[4] = (uint32_t)t->esito;
    p->n[5] = t->richieste;
    p->n[6] = t->connessioni;
    p->n[7] = t->apertura;
    xSemaphoreGive(blocco);
}

bool telemetria_da_mandare(void)
{
    if (!eventi) return false;
    if (!avvio_fatto) {
        rete_statistiche_t s;
        rete_statistiche(&s);
        if (s.ms_wifi) return true;   // il rapporto di avvio parte appena la wifi ha dato un indirizzo
    }
    for (int i = 0; i < MAX_EVENTI; i++) if (eventi[i].seq) return true;
    return false;
}

// ---- il messaggio per il PC ---------------------------------------------------------------------------

static bool agg(char *j, size_t cap, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static bool agg(char *j, size_t cap, size_t *n, const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = vsnprintf(j + *n, cap - *n, fmt, a);
    va_end(a);
    if (r < 0 || (size_t)r >= cap - *n) return false;
    *n += (size_t)r;
    return true;
}

static bool agg_testo(char *j, size_t cap, size_t *n, const char *s)
{
    for (; *s; s++) {
        const bool esc = *s == '"' || *s == '\\';
        if (*n + (esc ? 2u : 1u) + 1 > cap) return false;
        if (esc) j[(*n)++] = '\\';
        j[(*n)++] = *s;
    }
    j[*n] = 0;
    return true;
}

static bool scrivi_evento(char *j, size_t cap, size_t *n, const evento_t *e)
{
    if (e->tipo == 't') {
        return agg(j, cap, n, "{\"k\":\"tempi\",\"u\":%lu,\"inv\":%lu,\"att\":%lu,\"sca\":%lu,\"rip\":%lu,\"es\":%lu,\"rich\":%lu,\"conn\":%lu,\"tls\":%lu}",
                   (unsigned long)e->u, (unsigned long)e->n[0], (unsigned long)e->n[1], (unsigned long)e->n[2], (unsigned long)e->n[3],
                   (unsigned long)e->n[4], (unsigned long)e->n[5], (unsigned long)e->n[6], (unsigned long)e->n[7]);
    }
    if (e->tipo == 'a') {
        return agg(j, cap, n, "{\"k\":\"avvio\",\"u\":%lu,\"wifi\":%lu,\"rst\":%lu,\"mem\":%lu,\"blk\":%lu}",
                   (unsigned long)e->u, (unsigned long)e->n[0], (unsigned long)e->n[1], (unsigned long)e->n[2], (unsigned long)e->n[3]);
    }
    return agg(j, cap, n, "{\"k\":\"errore\",\"u\":%lu,\"o\":\"", (unsigned long)e->u)
           && agg_testo(j, cap, n, e->origine)
           && agg(j, cap, n, "\",\"x\":\"")
           && agg_testo(j, cap, n, e->testo)
           && agg(j, cap, n, "\"}");
}

bool telemetria_manda(void)
{
    if (!eventi) return true;
    char *json = heap_caps_malloc(CAP_JSON, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) return false;

    // Misure prese prima di bloccare la coda: la forza del segnale, e per il rapporto di avvio la wifi, il motivo dell'ultimo riavvio e la memoria.
    rete_statistiche_t st;
    rete_statistiche(&st);
    const int segnale = rete_segnale();
    const uint32_t rst = (uint32_t)esp_reset_reason();
    const uint32_t mem = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    const uint32_t blk = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    xSemaphoreTake(blocco, portMAX_DELAY);
    if (!avvio_fatto && st.ms_wifi) {
        evento_t *a = nuovo('a');
        a->n[0] = st.ms_wifi;
        a->n[1] = rst;
        a->n[2] = mem;
        a->n[3] = blk;
        avvio_fatto = true;
    }
    size_t n = 0;
    uint32_t ultimo = 0;
    int quanti = 0;
    const size_t cap_eventi = CAP_JSON - 4;   // resta posto per "]}" e lo zero
    bool ok = agg(json, cap_eventi, &n, "{\"t\":\"schermo\",\"v\":\"%.31s\",\"w\":%d,\"u\":%lu,\"ev\":[",
                  esp_app_get_description()->version, segnale, (unsigned long)ora_ms());
    while (ok) {
        const evento_t *p = NULL;   // il prossimo in ordine d'arrivo
        for (int i = 0; i < MAX_EVENTI; i++) if (eventi[i].seq > ultimo && (!p || eventi[i].seq < p->seq)) p = &eventi[i];
        if (!p) break;
        const size_t prima = n;
        if ((quanti > 0 && !agg(json, cap_eventi, &n, ",")) || !scrivi_evento(json, cap_eventi, &n, p)) { n = prima; json[n] = 0; break; }
        ultimo = p->seq;
        quanti++;
    }
    xSemaphoreGive(blocco);
    if (!ok || !agg(json, CAP_JSON, &n, "]}")) { free(json); return false; }
    if (quanti == 0) { free(json); return true; }   // niente da dire

    // Una richiesta sola, senza ritirare la risposta (come i pezzi intermedi della voce): il PC risponde lo stesso e il ponte
    // la butta. Se parte, quegli eventi escono dalla coda; altrimenti restano per la prossima volta.
    const ponte_esito_t e = ponte_manda(json, n, false, NULL, 0);
    free(json);
    if (e != PONTE_OK) { ESP_LOGW(TAG, "non partita (%d): %d eventi restano in coda", (int)e, quanti); return false; }
    xSemaphoreTake(blocco, portMAX_DELAY);
    for (int i = 0; i < MAX_EVENTI; i++) if (eventi[i].seq && eventi[i].seq <= ultimo) eventi[i].seq = 0;
    xSemaphoreGive(blocco);
    return true;
}

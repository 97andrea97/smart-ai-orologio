// Schermata Wifi (27/11/2026): aggiungere una rete dallo schermo, senza cavo (prima si poteva solo dalla seriale, e fuori
// casa l'orologio restava con "nessun wifi"). Si apre col tasto "Wifi" in alto a destra (main.c), mai durante la voce o
// lo scaricamento di un aggiornamento. Scansione, elenco per segnale, password con la tastiera di LVGL (visibile mentre
// si scrive: i tasti sono piccoli); Salva prova per prima la rete nuova e dice com'e' andata.
// Tutto qui gira nel compito di LVGL col blocco preso: le callback dei tocchi e un timer di LVGL, che legge le variabili
// scritte dal gestore degli eventi wifi in rete.c. I tocchi segnano solo cosa fare (richiesta) e lo fa il timer: cosi' un
// oggetto non si cancella dentro il suo stesso evento.
// La password non va mai nel registro ne' altrove: dal campo passa in un buffer, va in memoria (NVS), e buffer e campo si
// azzerano; anche annullando. Le reti gia' salvate si mostrano solo per nome. Gli oggetti si creano all'apertura e si
// cancellano alla chiusura (la memoria interna serve all'HTTPS); i dati stanno nella PSRAM.

#include "wifi_schermo.h"
#include "rete.h"
#include "memoria.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#define MAX_ELENCO          12      // reti in elenco, le piu' forti
#define PASSO_MS            100     // giro del timer
#define SCANSIONE_MAX_MS    15000   // una scansione di tutti i canali dura 2-4 s
#define ESITO_MAX_MS        30000   // il rifiuto di una rete arriva di solito entro 10 s
#define RIPROVA_SCANSIONE_MS 500    // fermato il giro, il driver puo' rifiutare la scansione per un attimo
#define TENTATIVI_SCANSIONE 8
#define PASSWORD_MIN        8       // WPA: da 8 a 64 caratteri
#define PASSWORD_MAX        64
#define COLORE_TASTO 0x1E4B5E
#define COLORE_RIGA  0x1E2A33
#define COLORE_TENUE 0x9AA5B1

typedef enum { AZ_NIENTE, AZ_CERCA, AZ_CHIUDI, AZ_RETE, AZ_SALVA, AZ_ANNULLA, AZ_RIPROVA, AZ_ELENCO, AZ_SOSTITUISCI } azione_t;
typedef enum { SC_FERMA, SC_DA_AVVIARE, SC_IN_CORSO } scansione_t;
typedef enum { ESITO_IN_CORSO, ESITO_OK, ESITO_NO } esito_t;

typedef struct {
    rete_vista_t trovate[MAX_ELENCO];
    int n_trovate;
    char salvate[MAX_RETI][33];   // solo i nomi
    int n_salvate;
    char scelta[33];
    bool scelta_aperta;
    char segreto[PASSWORD_MAX + 1];
} dati_t;

static dati_t *d = NULL;
static lv_obj_t *fondo = NULL, *etichetta = NULL, *elenco = NULL, *campo = NULL;
static lv_timer_t *timer = NULL;
static volatile bool aperta = false;
static azione_t richiesta = AZ_NIENTE;
static int indice = 0;
static scansione_t scansione = SC_FERMA;
static int tentativi = 0;
static uint32_t dal_ms = 0;
static bool attendo_esito = false;

static uint32_t adesso_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

bool wifi_schermo_aperto(void) { return aperta; }

// ---- pezzi dello schermo -----------------------------------------------------------------------------

#define DATO(az, i) ((void *)(intptr_t)((int)(az) | ((int)(i) << 8)))

static void tocco(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    richiesta = (azione_t)(v & 0xFF);
    indice = (int)(v >> 8);
}

// La tastiera: il suo tasto di conferma vale Salva, quello della tastiera vale Annulla.
static void tocco_tastiera(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) richiesta = AZ_SALVA;
    else if (c == LV_EVENT_CANCEL) richiesta = AZ_ANNULLA;
}

static void tasto(const char *testo, azione_t az, lv_align_t dove, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_button_create(fondo);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, dove, x, y);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLORE_TASTO), 0);
    lv_obj_add_event_cb(b, tocco, LV_EVENT_CLICKED, DATO(az, 0));
    lv_obj_t *s = lv_label_create(b);
    lv_label_set_text(s, testo);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_24, 0);
    lv_obj_center(s);
}

static lv_obj_t *scritta(int y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(fondo);
    lv_obj_set_width(l, 390);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *nuovo_elenco(int y, int h)
{
    lv_obj_t *l = lv_list_create(fondo);
    lv_obj_set_size(l, 390, h);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_row(l, 8, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    return l;
}

static void riga(const char *testo, azione_t az, int i)
{
    lv_obj_t *b = lv_list_add_button(elenco, NULL, testo);
    lv_obj_set_height(b, 64);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLORE_RIGA), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_event_cb(b, tocco, LV_EVENT_CLICKED, DATO(az, i));
}

// Il testo del campo si azzera nel buffer dell'etichetta di LVGL prima di svuotarlo: non resta nella memoria liberata.
static void svuota_campo(void)
{
    if (!campo) return;
    char *t = (char *)lv_label_get_text(lv_textarea_get_label(campo));
    if (t) memset(t, 0, strlen(t));
    lv_textarea_set_text(campo, "");
    campo = NULL;
}

static void cancella_segreto(void)
{
    if (d) memset(d->segreto, 0, sizeof d->segreto);
}

static void nuova_vista(void)
{
    svuota_campo();
    lv_obj_clean(fondo);
    etichetta = elenco = NULL;
}

// Motivi di wifi_err_reason_t (esp_wifi_types), in parole; NULL se non e' uno di questi.
static const char *motivo_testo(int m)
{
    switch (m) {
        case 15: case 204: return "password sbagliata";
        case 201: return "rete non trovata";
        case 2: case 202: return "accesso rifiutato dalla rete";
        case 210: case 211: return "protezione della rete non compatibile";
        case 212: return "segnale troppo debole";
        default: return NULL;
    }
}

// ---- viste ---------------------------------------------------------------------------------------------

static void mostra_trovate(void)
{
    if (!elenco) return;
    lv_obj_clean(elenco);
    for (int i = 0; i < d->n_trovate; i++) {
        char testo[64];
        snprintf(testo, sizeof testo, "%s%s", d->trovate[i].ssid, d->trovate[i].aperta ? "  (aperta)" : "");
        riga(testo, AZ_RETE, i);
    }
}

static void vista_elenco(bool cerca)
{
    nuova_vista();
    lv_obj_t *titolo = scritta(8, &lv_font_montserrat_24);
    lv_label_set_text(titolo, "Wifi");
    etichetta = scritta(40, &lv_font_montserrat_16);
    lv_obj_set_style_text_color(etichetta, lv_color_hex(COLORE_TENUE), 0);
    elenco = nuovo_elenco(66, 340);
    tasto("Cerca", AZ_CERCA, LV_ALIGN_BOTTOM_LEFT, 15, -14, 180, 60);
    tasto("Indietro", AZ_CHIUDI, LV_ALIGN_BOTTOM_RIGHT, -15, -14, 180, 60);
    if (cerca) {
        d->n_trovate = 0;
        scansione = SC_DA_AVVIARE;
        tentativi = 0;
        dal_ms = adesso_ms() - RIPROVA_SCANSIONE_MS;   // il primo tentativo subito
        lv_label_set_text(etichetta, "Cerco le reti...");
    } else {
        mostra_trovate();
        lv_label_set_text_fmt(etichetta, "%d reti: tocca quella da aggiungere", d->n_trovate);
    }
}

static void vista_password(const char *avviso)
{
    nuova_vista();
    etichetta = scritta(4, &lv_font_montserrat_18);
    if (avviso) lv_label_set_text(etichetta, avviso);
    else lv_label_set_text_fmt(etichetta, "Password di %s", d->scelta);
    campo = lv_textarea_create(fondo);
    lv_obj_set_size(campo, 390, 54);
    lv_obj_align(campo, LV_ALIGN_TOP_MID, 0, 50);
    lv_textarea_set_one_line(campo, true);
    lv_textarea_set_password_mode(campo, false);   // visibile mentre si scrive: i tasti sono piccoli
    lv_textarea_set_max_length(campo, PASSWORD_MAX);
    lv_obj_set_style_text_font(campo, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(campo, lv_color_hex(COLORE_RIGA), 0);
    lv_obj_set_style_bg_opa(campo, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(campo, lv_color_white(), 0);
    lv_obj_add_state(campo, LV_STATE_FOCUSED);   // si vede il cursore
    tasto("Salva", AZ_SALVA, LV_ALIGN_TOP_LEFT, 15, 110, 180, 52);
    tasto("Annulla", AZ_ANNULLA, LV_ALIGN_TOP_RIGHT, -15, 110, 180, 52);
    // Tastiera di LVGL: minuscole, maiuscole (ABC), numeri e simboli (1#).
    lv_obj_t *k = lv_keyboard_create(fondo);
    lv_obj_set_size(k, 410, 334);
    lv_obj_align(k, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_18, 0);
    lv_keyboard_set_textarea(k, campo);
    lv_obj_add_event_cb(k, tocco_tastiera, LV_EVENT_ALL, NULL);
}

static void vista_piena(void)
{
    nuova_vista();
    etichetta = scritta(8, &lv_font_montserrat_18);
    lv_label_set_text_fmt(etichetta, "Spazi pieni (%d reti salvate).\nTocca quella da sostituire con %s", MAX_RETI, d->scelta);
    elenco = nuovo_elenco(84, 320);
    for (int i = 0; i < d->n_salvate; i++) riga(d->salvate[i], AZ_SOSTITUISCI, i);
    tasto("Annulla", AZ_ANNULLA, LV_ALIGN_BOTTOM_MID, 0, -14, 220, 60);
}

static void vista_esito(esito_t e, const char *testo)
{
    nuova_vista();
    etichetta = scritta(150, &lv_font_montserrat_24);
    lv_label_set_text(etichetta, testo);
    if (e == ESITO_IN_CORSO) tasto("Indietro", AZ_CHIUDI, LV_ALIGN_BOTTOM_MID, 0, -14, 220, 60);
    else if (e == ESITO_OK) tasto("Fine", AZ_CHIUDI, LV_ALIGN_BOTTOM_MID, 0, -14, 220, 60);
    else if (d->scelta_aperta) {   // rete aperta: niente da correggere, si riparte dall'elenco
        tasto("Elenco", AZ_ELENCO, LV_ALIGN_BOTTOM_LEFT, 15, -14, 180, 60);
        tasto("Indietro", AZ_CHIUDI, LV_ALIGN_BOTTOM_RIGHT, -15, -14, 180, 60);
    } else {
        tasto("Riprova", AZ_RIPROVA, LV_ALIGN_BOTTOM_LEFT, 15, -14, 180, 60);
        tasto("Elenco", AZ_ELENCO, LV_ALIGN_BOTTOM_RIGHT, -15, -14, 180, 60);
    }
}

// ---- salvataggio ----------------------------------------------------------------------------------------

// Nomi delle reti salvate in d->salvate; le password lette insieme si azzerano subito.
static int leggi_salvate(void)
{
    rete_t r[MAX_RETI];
    int n = memoria_reti(r);
    for (int i = 0; i < n; i++) memcpy(d->salvate[i], r[i].ssid, sizeof d->salvate[i]);
    memset(r, 0, sizeof r);
    d->n_salvate = n;
    return n;
}

static void prova(void)
{
    cancella_segreto();
    rete_ricarica_prima(d->scelta);
    attendo_esito = true;
    dal_ms = adesso_ms();
    char testo[96];
    snprintf(testo, sizeof testo, "Provo %s...", d->scelta);
    vista_esito(ESITO_IN_CORSO, testo);
}

static void registra(void)
{
    if (memoria_aggiungi_rete(d->scelta, d->segreto)) { prova(); return; }
    int n = leggi_salvate();
    bool gia = false;
    for (int i = 0; i < n; i++) if (strcmp(d->salvate[i], d->scelta) == 0) gia = true;
    if (n >= MAX_RETI && !gia) { vista_piena(); return; }   // la password resta nel buffer finche' non si sceglie
    cancella_segreto();
    vista_esito(ESITO_NO, "Rete non salvata: memoria dell'orologio non scrivibile");
}

static void salva(void)
{
    if (!d->scelta_aperta) {
        if (!campo) return;
        const char *t = lv_textarea_get_text(campo);
        size_t n = t ? strlen(t) : 0;
        if (n < PASSWORD_MIN || n > PASSWORD_MAX) {
            lv_label_set_text(etichetta, "Almeno 8 caratteri: correggi e tocca Salva");
            return;
        }
        memcpy(d->segreto, t, n);
        d->segreto[n] = 0;
    } else {
        d->segreto[0] = 0;
    }
    svuota_campo();
    registra();
}

// ---- giro del timer ---------------------------------------------------------------------------------------

static void segui_scansione(void)
{
    uint32_t ora = adesso_ms();
    if (scansione == SC_DA_AVVIARE) {
        if (ora - dal_ms < RIPROVA_SCANSIONE_MS) return;
        dal_ms = ora;
        if (rete_scansiona()) { scansione = SC_IN_CORSO; return; }
        if (++tentativi >= TENTATIVI_SCANSIONE) {
            scansione = SC_FERMA;
            if (etichetta) lv_label_set_text(etichetta, "Scansione non riuscita: tocca Cerca");
        }
    } else if (scansione == SC_IN_CORSO) {
        bool ok = false;
        if (rete_scansione_finita(&ok)) {
            scansione = SC_FERMA;
            if (ok) d->n_trovate = rete_scansione_leggi(d->trovate, MAX_ELENCO);
            else { d->n_trovate = 0; rete_scansione_ferma(); }
            mostra_trovate();
            if (!etichetta) return;
            if (!ok) lv_label_set_text(etichetta, "Scansione non riuscita: tocca Cerca");
            else if (d->n_trovate == 0) lv_label_set_text(etichetta, "Nessuna rete trovata: tocca Cerca");
            else lv_label_set_text_fmt(etichetta, "%d reti: tocca quella da aggiungere", d->n_trovate);
        } else if (ora - dal_ms > SCANSIONE_MAX_MS) {
            scansione = SC_FERMA;
            rete_scansione_ferma();
            if (etichetta) lv_label_set_text(etichetta, "Scansione senza risposta: tocca Cerca");
        }
    }
}

static void segui_esito(void)
{
    int e = rete_esito_nuova();
    char testo[160];
    if (e == 0) {
        attendo_esito = false;
        snprintf(testo, sizeof testo, "Collegato a %s", d->scelta);
        vista_esito(ESITO_OK, testo);
    } else if (e > 0) {
        attendo_esito = false;
        const char *m = motivo_testo(e);
        if (m) snprintf(testo, sizeof testo, "Non collegato a %s:\n%s", d->scelta, m);
        else snprintf(testo, sizeof testo, "Non collegato a %s:\nmotivo %d", d->scelta, e);
        vista_esito(ESITO_NO, testo);
    } else if (adesso_ms() - dal_ms > ESITO_MAX_MS) {
        attendo_esito = false;
        snprintf(testo, sizeof testo, "Nessuna risposta da %s", d->scelta);
        vista_esito(ESITO_NO, testo);
    }
}

static void chiudi(void)
{
    if (!fondo) return;
    if (scansione != SC_FERMA) rete_scansione_ferma();
    scansione = SC_FERMA;
    attendo_esito = false;
    svuota_campo();
    if (timer) { lv_timer_delete(timer); timer = NULL; }
    lv_obj_delete(fondo);
    fondo = etichetta = elenco = NULL;
    if (d) { memset(d, 0, sizeof *d); free(d); d = NULL; }
    rete_pausa(false);   // il giro fra le reti salvate riparte (se dopo Salva non e' gia' ripartito)
    aperta = false;
}

static void passo(lv_timer_t *t)
{
    (void)t;
    azione_t r = richiesta;
    richiesta = AZ_NIENTE;
    switch (r) {
        case AZ_CHIUDI: chiudi(); return;
        case AZ_CERCA: if (scansione == SC_FERMA) vista_elenco(true); break;
        case AZ_RETE:
            if (indice < 0 || indice >= d->n_trovate || scansione != SC_FERMA) break;
            memcpy(d->scelta, d->trovate[indice].ssid, sizeof d->scelta);
            d->scelta_aperta = d->trovate[indice].aperta;
            if (d->scelta_aperta) salva();   // rete aperta: si salva senza password
            else vista_password(NULL);
            break;
        case AZ_SALVA: salva(); break;
        case AZ_ANNULLA: cancella_segreto(); vista_elenco(false); break;
        case AZ_RIPROVA: rete_pausa(true); vista_password(NULL); break;
        case AZ_ELENCO: rete_pausa(true); vista_elenco(true); break;
        case AZ_SOSTITUISCI:
            if (indice < 0 || indice >= d->n_salvate) break;
            memoria_togli_rete(d->salvate[indice]);
            registra();
            break;
        default: break;
    }
    segui_scansione();
    if (attendo_esito) segui_esito();
}

void wifi_schermo_apri(void)
{
    if (fondo) return;
    d = heap_caps_calloc(1, sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return;
    // Sopra tutto lo schermo principale; il velo dello schermo spento (lv_layer_top) resta sopra anche a questa.
    fondo = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(fondo);
    lv_obj_set_size(fondo, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(fondo, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fondo, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(fondo, lv_color_white(), 0);
    lv_obj_add_flag(fondo, LV_OBJ_FLAG_CLICKABLE);   // i tocchi non arrivano ai tasti sotto
    lv_obj_remove_flag(fondo, LV_OBJ_FLAG_SCROLLABLE);
    richiesta = AZ_NIENTE;
    attendo_esito = false;
    aperta = true;
    rete_pausa(true);
    timer = lv_timer_create(passo, PASSO_MS, NULL);
    vista_elenco(true);
}

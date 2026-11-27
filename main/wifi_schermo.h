// Schermata Wifi dell'orologio (27/11/2026, wifi_schermo.c): aggiungere una rete dallo schermo, senza cavo.
#pragma once
#include <stdbool.h>

// Apre la schermata: va chiamata col blocco di LVGL preso (per esempio dalla callback di un tocco). Si chiude da sola
// col suo tasto Indietro o Fine.
void wifi_schermo_apri(void);
// Vero mentre la schermata e' aperta; si legge da ogni compito.
bool wifi_schermo_aperto(void);

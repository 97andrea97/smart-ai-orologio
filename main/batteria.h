// Batteria: percentuale e carica dal chip AXP2101 (batteria.c).
#pragma once
#include <stdbool.h>

void batteria_avvia(void);                            // dopo bsp_display_start (bus I2C condiviso col touch)
bool batteria_leggi(int *percento, bool *in_carica);  // false se la batteria o il chip non rispondono
bool batteria_tensione(int *mv);                      // tensione della batteria in millivolt; false se non risponde
bool batteria_tasto_pwr(void);                        // true una volta per ogni pressione breve di PWR

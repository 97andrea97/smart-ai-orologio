// Batteria: percentuale e carica dal chip AXP2101 (batteria.c).
#pragma once
#include <stdbool.h>

void batteria_avvia(void);                            // dopo bsp_display_start (bus I2C condiviso col touch)
bool batteria_leggi(int *percento, bool *in_carica);  // false se la batteria o il chip non rispondono

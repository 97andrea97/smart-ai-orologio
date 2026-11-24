// Abbinamento col cavo: la pagina Orologio dell'app (Chrome, Web Serial) manda una riga JSON per
// comando; l'orologio risponde con una riga che comincia per "@@" (il resto del flusso sono i log).
//   {"cmd":"info"}                                       -> versione, abbinato, nomi delle reti
//   {"cmd":"abbina","ponte":"https://...","gettone":"<64 hex>","chiave":"<64 hex>"}
//   {"cmd":"wifi","ssid":"...","password":"..."}         -> aggiunge o sostituisce una rete
//   {"cmd":"wifi_togli","ssid":"..."}
//   {"cmd":"riavvia"}
// Le risposte non ripetono mai gettone, chiave o password.
#pragma once

// `cambiato` viene chiamata (fuori dal compito di LVGL) dopo abbina e wifi, per aggiornare lo schermo.
void seriale_avvia(void (*cambiato)(void));

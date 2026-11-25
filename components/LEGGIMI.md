# Pacchetto della scheda: copia corretta di Waveshare

`waveshare__esp32_s3_touch_amoled_2_06\` e' copiato TALE E QUALE da
github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06, cartella
`examples/factory/components/waveshare__esp32_s3_touch_amoled_2_06` (ultimo cambio del file
principale: commit 30f3b23 del 20/10/2026; copiato dal commit 3de8684 il 25/11/2026), licenza
Apache 2.0 (file LICENSE accanto). Col nome della cartella prende il posto del pacchetto 2.0.0
del registro Espressif, come fa il firmware di fabbrica di Waveshare.

Perche': la 2.0.0 del registro apre lo schermo (QSPI) con la via pensata per gli schermi RGB, che
dichiara finito un invio appena partito, e tiene il disegno in PSRAM. Col wifi acceso gli invii
grandi falliscono ("spi transmit (queue) color failed"): fasce o schermo verde, visti
sull'orologio vero il 25/11/2026. La copia di fabbrica usa la via QSPI (aspetta la fine di ogni
invio) e la memoria interna adatta ai trasferimenti.

Quando Waveshare pubblichera' una versione corretta nel registro, si toglie questa cartella.
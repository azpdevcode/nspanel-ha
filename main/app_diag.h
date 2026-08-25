/*
 * Diagnostic mémoire léger. Sans PSRAM, la RAM interne est le seul tas : un
 * nouveau consommateur peut faire échouer un xTaskCreate ailleurs, sans bruit.
 * Ce module objective la marge réelle (plus bas niveau jamais atteint) au boot
 * puis périodiquement, pour détecter une régression AVANT la panne.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Trace l'état du tas avec un libellé de phase. */
void app_diag_log_mem(const char *phase);

/* Démarre le moniteur mémoire périodique (esp_timer, 30 s). Sans tâche dédiée. */
void app_diag_start(void);

#ifdef __cplusplus
}
#endif

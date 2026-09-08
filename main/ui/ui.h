/*
 * Tâche d'interface : décide quelle page afficher et y pousse les valeurs.
 *
 * Seul endroit qui écrit l'écran en régime normal. app_main() ne garde que son
 * battement de cœur console ; les transferts (app_ota, tft_ota) écrivent aussi,
 * mais le verrou du bus (nx_bus_acquire) et ui_screen_yield() les arbitrent.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Démarre la tâche d'interface. À appeler après nextion_init(). */
esp_err_t ui_start(void);

/** @brief Suspend (true) ou reprend (false) les écritures périodiques de
 *  ui_task (home_page_tick/cfg_page_tick), sans toucher au drainage tactile
 *  ni à la détection d'inactivité. Sert à laisser l'écran tranquille pendant
 *  une manipulation manuelle via /nx (ex. une mesure `sendxy`) : sans ça, une
 *  resynchronisation périodique (toutes les 20 s, voir resync_due() dans
 *  ui.c) réécrit des objets `cfg.*`/`home.*` par-dessus l'écran. Ne sert PAS
 *  à faire fonctionner `touch_j` en commande série runtime — cette instruction
 *  ne s'exécute que placée dans l'événement Page Preinitialize d'un projet
 *  compilé, jamais envoyée après coup (voir design/HMI-SPEC.md § Biais
 *  tactile). */
void ui_freeze(bool on);

/* --- Journal tactile ---
 *
 * Les hotspots de navigation ne sont pas dans nx_objects.def : ils sont
 * identifiés par le `id` que l'Editor leur attribue, qui n'est écrit nulle
 * part dans les sources et change si on supprime/recrée un composant. Plutôt
 * que de le relever à la main, on garde les derniers appuis reçus et on les
 * expose sur /status : toucher la zone suffit à lire son id. */
typedef struct {
    uint8_t  page;
    uint8_t  cid;
    bool     pressed;
    uint32_t uptime_s;
} ui_touch_rec_t;

/** @brief Copie jusqu'à `max` appuis, du plus récent au plus ancien.
 *  @return le nombre effectivement copié. */
int ui_touch_history(ui_touch_rec_t *out, int max);

#ifdef __cplusplus
}
#endif

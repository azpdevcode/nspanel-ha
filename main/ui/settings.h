/*
 * Réglages persistés du panneau — allégé de ha-panel/main/ui/home_ui.c:318-432.
 * Namespace NVS `nspanel` (celui de app_config.c), pas `ha_panel`.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Charge les réglages depuis NVS et applique leurs effets de bord matériels
 *  (nx_dim, thsp/thup). À appeler une fois au boot, après nextion_init(). */
void settings_load(void);

/** Nom de la pièce par défaut (persistance par NOM : l'index HA change). */
const char *settings_default_room(void);
void settings_save_default_room(const char *name);

int  settings_brightness(void);      /* 0-100 % */
void settings_save_brightness(int pct);

int  settings_sleep_enabled(void);   /* 0/1 */
void settings_save_sleep_enabled(int enabled);

int  settings_sleep_timeout_s(void);
void settings_save_sleep_timeout(int sec);

#ifdef __cplusplus
}
#endif

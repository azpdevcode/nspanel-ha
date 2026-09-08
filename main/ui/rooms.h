/*
 * Modèle de pièces — allégé de ha-panel/main/ui/home_ui.c:443-593 +
 * home_internal.h:36-41 (rooms/lights), sans LVGL ni Music Assistant.
 *
 * Téléchargé une fois au boot (et périodiquement) via un template Jinja HA :
 * chaque aire ayant au moins une lumière étiquetée « Light ha panel », avec ses
 * lumières et son entité climate (posée pour la phase 3). Alimente aussi le
 * worker HA (ha_worker_set_tracked) pour le compteur d'accueil.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROOMS_MAX_ROOMS   8
#define ROOMS_MAX_LIGHTS  8

/** Démarre (ou relance, si déjà en cours : no-op) le téléchargement du modèle
 *  de pièces depuis HA. Non bloquant, réentrant. */
void rooms_start_fetch(void);

/** Nombre de pièces actuellement connues (0 tant que le premier fetch n'a pas
 *  abouti). */
int rooms_count(void);

/** Nom de la pièce `i` (chaîne stable jusqu'au prochain fetch réussi), ou "" si
 *  hors bornes. */
const char *rooms_name(int i);

/** Nombre de lumières de la pièce `i`. */
int rooms_light_count(int i);

/** Entité climate.* de la pièce `i` (chaîne vide si aucune). */
const char *rooms_climate_entity(int i);

/** Dernier état climate connu de la pièce `i` (poll de secours ou push WS).
 *  `*ok` est false tant qu'aucun état n'est arrivé, ou si l'entité HA est
 *  `unavailable` — distingue « pas encore su » de « 0 °C ». */
void rooms_climate_state(int i, float *cur_temp, float *set_temp, bool *ok);

/** Pièce actuellement affichée par l'écran lumières (0 par défaut). */
int  rooms_current(void);
void rooms_set_current(int i);

/** Bascule toutes les lumières de la pièce actuellement affichée : si l'une
 *  d'elles au moins est allumée, les éteint toutes ; sinon les allume toutes.
 *  No-op si la pièce n'a aucune lumière ou si le modèle n'est pas encore
 *  chargé. */
void rooms_toggle_current(void);

/** Nombre total de lumières suivies / actuellement allumées (toutes pièces
 *  confondues), pour le compteur d'accueil (tLights). */
int rooms_lights_total(void);
int rooms_lights_on(void);

/** true si le modèle a changé depuis le dernier appel (consomme le drapeau) —
 *  la tâche ui l'utilise pour savoir quand réécrire tRoom/tLights. */
bool rooms_model_dirty(void);

/** true si un fetch est en cours. */
bool rooms_fetching(void);

/** Démarre la tâche de fond qui relance rooms_start_fetch() périodiquement
 *  (~30 min) et à la reconnexion WebSocket. À appeler une fois après
 *  ha_worker_start() (elle enregistre le callback d'état lumière) et
 *  ha_ws_start(). */
esp_err_t rooms_watchdog_start(void);

#ifdef __cplusplus
}
#endif

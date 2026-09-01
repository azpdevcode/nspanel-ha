/*
 * Worker HA : une seule task sérialise tout le trafic REST bloquant vers Home
 * Assistant (lumières uniquement dans cette étape — chauffage/météo suivront en
 * phase 3). Ne touche JAMAIS l'UART écran : c'est la tâche `ui` qui lit le
 * dernier état connu via le callback ci-dessous et rend.
 *
 * Allégé de ha-panel/main/ui/ha_worker.c (720 l.) : plus de média, de créneau
 * climate ni de journal HA. Les entités suivies ne sont pas déclarées page par
 * page (« on_resume »/« on_pause ») : rooms.c pousse la liste complète des
 * lumières connues (s_glights, 16 max) une fois par pièces-fetch, et elles
 * restent suivies en permanence — c'est ce qui alimente le compteur d'accueil.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ha_worker_start(void);   /* crée la task ; attend le WiFi */

/** Remplace la liste des entités suivies (poll de secours + push WS). count
 *  borné à 16. Poste une sync immédiate si count > 0. */
void ha_worker_set_tracked(const char *const *entity_ids, int count);

void ha_worker_request_toggle(const char *entity_id);          /* homeassistant/toggle */
void ha_worker_request_set(const char *entity_id, bool on);    /* turn_on / turn_off */
void ha_worker_request_brightness(const char *entity_id, int pct);   /* 1..100 %, light.* seulement */

/* Notifié (depuis la task worker) à chaque état lumière connu, par poll OU par
 * push WS — un seul callback global, à charge de l'appelant (rooms.c) de
 * retrouver l'entité dans son propre modèle. */
typedef void (*ha_light_state_cb_t)(const char *entity_id, bool on, void *user);
void ha_worker_set_light_cb(ha_light_state_cb_t cb, void *user);

/* ---- Push temps réel (WebSocket) ----
 * ha_ws filtre les events HA via ha_worker_tracks(), puis transmet ceux qui
 * nous concernent via ha_worker_post_state_event(). Dispatch sur la task
 * worker → jamais de concurrence avec le poll. */
bool ha_worker_tracks(const char *entity_id);
void ha_worker_post_state_event(const char *entity_id, char *json);   /* prend possession de json */

/** Met le poll réseau en pause (écran en veille). Les push WS restent traités. */
void ha_worker_pause_poll(bool pause);

#ifdef __cplusplus
}
#endif

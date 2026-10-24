/*
 * Worker HA : une seule task sérialise tout le trafic REST bloquant vers Home
 * Assistant (lumières, chauffage, météo). Ne touche
 * JAMAIS l'UART écran : c'est la tâche `ui` qui lit le dernier état connu via
 * les callbacks ci-dessous et rend.
 *
 * Allégé de ha-panel/main/ui/ha_worker.c (720 l.) : plus de média ni de journal
 * HA. Les entités suivies ne sont pas déclarées page par page (« on_resume »/
 * « on_pause ») : rooms.c pousse la liste complète des lumières et des entités
 * climate connues une fois par pièces-fetch, et elles restent suivies en
 * permanence — c'est ce qui alimente le compteur d'accueil et les anneaux de
 * température.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ha_client.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ha_worker_start(void);   /* crée la task ; attend le WiFi */

/** Remplace la liste des entités suivies (poll de secours + push WS) — un seul
 *  répertoire pour light./switch. ET climate., distingués par préfixe
 *  d'entity_id. count borné à 24. Poste une sync immédiate si count > 0. */
void ha_worker_set_tracked(const char *const *entity_ids, int count);

void ha_worker_request_toggle(const char *entity_id);          /* homeassistant/toggle */
void ha_worker_request_set(const char *entity_id, bool on);    /* turn_on / turn_off */
void ha_worker_request_brightness(const char *entity_id, int pct);   /* 1..100 %, light.* seulement */

/* Notifié (depuis la task worker) à chaque état lumière connu, par poll OU par
 * push WS — un seul callback global, à charge de l'appelant (rooms.c) de
 * retrouver l'entité dans son propre modèle. */
typedef void (*ha_light_state_cb_t)(const char *entity_id, bool on, void *user);
void ha_worker_set_light_cb(ha_light_state_cb_t cb, void *user);

/* Même motif pour le chauffage : un seul callback global, appelé à chaque état
 * climate connu (poll OU push WS). `st` n'est valide que le temps de l'appel. */
typedef void (*ha_climate_state_cb_t)(const char *entity_id,
                                      const ha_climate_state_t *st, void *user);
void ha_worker_set_climate_cb(ha_climate_state_cb_t cb, void *user);

/* Météo : une seule entité fixe (CONFIG_HA_WEATHER_ENTITY), pas de
 * répertoire par pièce comme light./climate. — posée à part de
 * ha_worker_set_tracked(), sondée en continu dès qu'elle est renseignée. */
void ha_worker_set_weather_entity(const char *entity_id);
typedef void (*ha_weather_state_cb_t)(const ha_weather_state_t *st, void *user);
void ha_worker_set_weather_cb(ha_weather_state_cb_t cb, void *user);

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

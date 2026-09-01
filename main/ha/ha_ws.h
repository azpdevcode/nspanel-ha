#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Client WebSocket Home Assistant — temps réel (push) au lieu du poll REST.
 *
 * Allégé de ha-panel/main/ha/ha_ws.c : pipeline vocale, notifications persistantes
 * et interphone retirés (voir design/FORK-PLAN.md). Il ne reste qu'un seul
 * abonnement, `state_changed`, dispatché vers ha_worker_post_state_event().
 *
 * Cycle géré en interne :
 *   1. connexion ws(s)://<host>/api/websocket
 *   2. handshake d'authentification : HA envoie `auth_required`, on répond avec
 *      le token longue durée (`auth`), HA confirme par `auth_ok`.
 *   3. abonnement aux changements d'état (`subscribe_events` / `state_changed`).
 *   4. réception des events poussés par HA, filtrés par ha_worker_tracks().
 *
 * La reconnexion automatique (et la ré-auth + ré-abonnement) est gérée par
 * esp_websocket_client + la machine à états interne.
 */
esp_err_t ha_ws_start(void);

/** @brief true si la session WS est authentifiée ET abonnée à state_changed. */
bool ha_ws_is_ready(void);

/** @brief true si auth_ok a été reçu (indépendamment de l'abonnement). */
bool ha_ws_is_authed(void);

/** @brief true si le message auth_invalid a été reçu au dernier essai
 *  (token rejeté par HA — distinct d'une simple coupure réseau). */
bool ha_ws_auth_invalid(void);

/** @brief Compteurs de diagnostic, exposés par /status (dev_portal). */
typedef struct {
    uint32_t events_received;    /* state_changed effectivement dispatchés */
    uint32_t frames_truncated;   /* frames > HA_WS_RX_CAP, event perdu */
    uint32_t reconnects;         /* WEBSOCKET_EVENT_DISCONNECTED observés */
} ha_ws_stats_t;
void ha_ws_get_stats(ha_ws_stats_t *out);

/** Envoie une commande JSON (texte) sur le WebSocket. */
esp_err_t ha_ws_send_cmd(const char *json);

/** Force la fermeture + reconnexion du WebSocket. À appeler quand un envoi échoue
 *  alors que la session se croit authentifiée (socket half-open que le ping/pong
 *  n'a pas encore coupé). Interdit depuis la task WS (callback de dispatch). */
void ha_ws_force_reconnect(void);

/* ---- Requête/réponse générique (corrélation par id) ----
 * Pour les commandes HA qui attendent UN `result` (ex. get_forecasts en phase 3
 * pour ha_weather). Le callback reçoit le message complet (root) et s'exécute sur
 * la task WS. One-shot : le slot est libéré juste après l'appel. */
typedef void (*ha_ws_result_cb_t)(cJSON *msg, void *user);

/** Alloue un id de commande croissant (HA exige des id croissants). */
int ha_ws_alloc_id(void);

/** Arme un callback one-shot pour le `result` d'id `id`, puis envoie `json`
 *  (qui doit contenir ce même `id`). Renvoie ESP_FAIL si aucun slot libre ou
 *  envoi échoué. */
esp_err_t ha_ws_request(int id, const char *json, ha_ws_result_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

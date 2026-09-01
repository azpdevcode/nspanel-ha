/*
 * Client WebSocket Home Assistant (push temps réel).
 *
 * S'appuie sur esp_websocket_client (composant managé) pour le transport, le
 * framing et la reconnexion automatique. Au-dessus, on implémente le protocole
 * applicatif HA : handshake d'auth par token, puis abonnement à `state_changed`.
 *
 * Allégé de ha-panel/main/ha/ha_ws.c (491 l.) : pipeline vocale, notifications
 * persistantes et interphone retirés — un seul abonnement (state_changed),
 * dispatché vers ha_worker. Voir design/FORK-PLAN.md, section « Stripping ».
 */
#include "ha_ws.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "app_config.h"

#include "ha_worker.h"

static const char *TAG = "ha_ws";

/* Frame texte HA : un state_changed complet tient largement dans 8 Ko (pas de
 * forecast/statistics ici, contrairement à ha-panel). En heap interne — pas de
 * PSRAM sur l'ESP32-D0WD : une frame plus grosse est tronquée, l'event perdu,
 * comptabilisé dans ha_ws_stats_t.frames_truncated (rattrapé par le poll de
 * secours de ha_worker). */
#define HA_WS_RX_CAP   (8 * 1024)

#define HA_WS_SUB_ID   1   /* subscribe_events(state_changed), seul abonnement */

static esp_websocket_client_handle_t s_client;
static char *s_rx;        /* accumulateur de la frame courante */
static int   s_rx_len;
/* Écrits par la task du client WS (on_ws_event), lus par d'autres tasks
 * (dev_portal /status) → volatile pour la visibilité inter-task. Bool 32-bit
 * aligné = accès atomique, pas besoin de mutex. */
static volatile bool s_authed;
static volatile bool s_subscribed;
static volatile bool s_auth_invalid;
static int s_msg_id = 1;   /* HA_WS_SUB_ID consomme le 1er */

static volatile uint32_t s_events_received;
static volatile uint32_t s_frames_truncated;
static volatile uint32_t s_reconnects;

/* Requêtes génériques en vol (posé pour ha_weather en phase 3) : corrélation
 * par id. Table fixe, peu de requêtes simultanées. */
#define HA_WS_REQ_SLOTS 4
static struct { int id; ha_ws_result_cb_t cb; void *user; } s_req[HA_WS_REQ_SLOTS];
static SemaphoreHandle_t s_req_mtx;

bool ha_ws_is_ready(void) { return s_authed && s_subscribed; }
bool ha_ws_is_authed(void) { return s_authed; }
bool ha_ws_auth_invalid(void) { return s_auth_invalid; }

void ha_ws_get_stats(ha_ws_stats_t *out)
{
    if (!out) return;
    out->events_received = s_events_received;
    out->frames_truncated = s_frames_truncated;
    out->reconnects = s_reconnects;
}

/* Construit ws(s)://host:port/api/websocket à partir de l'URL HA (app_config). */
static void build_ws_uri(char *out, size_t len)
{
    const char *base = app_config_ha_url();   /* ex. http://192.168.50.89:8123 */
    const char *scheme, *rest;
    if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://"; rest = base + 8;
    } else if (strncmp(base, "http://", 7) == 0) {
        scheme = "ws://";  rest = base + 7;
    } else {
        scheme = "ws://";  rest = base;
    }
    snprintf(out, len, "%s%s/api/websocket", scheme, rest);
}

static void ws_send(const char *json)
{
    if (!s_client) return;
    int n = (int)strlen(json);
    /* Timeout borné : un envoi qui ne part jamais ne doit pas geler la tâche WS
     * (elle traite aussi la réception → un blocage ici arrête tout le client). */
    int sent = esp_websocket_client_send_text(s_client, json, n, pdMS_TO_TICKS(2000));
    if (sent < 0) ESP_LOGW(TAG, "envoi WS échoué (%d)", sent);
}

esp_err_t ha_ws_send_cmd(const char *json)
{
    if (!s_client || !json) return ESP_FAIL;
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(2000));
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

/* Force une reconnexion : un envoi échoué alors qu'on se croit encore authentifié
 * révèle un socket half-open que le ping/pong n'a pas encore coupé. */
void ha_ws_force_reconnect(void)
{
    if (!s_client) return;
    s_authed = s_subscribed = false;
    ESP_LOGW(TAG, "reconnexion forcée (socket half-open suspecté)");
    esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "redémarrage WS après close échoué: %s", esp_err_to_name(err));
}

int ha_ws_alloc_id(void)
{
    int id;
    if (s_req_mtx) xSemaphoreTake(s_req_mtx, portMAX_DELAY);
    id = ++s_msg_id;
    if (s_req_mtx) xSemaphoreGive(s_req_mtx);
    return id;
}

esp_err_t ha_ws_request(int id, const char *json, ha_ws_result_cb_t cb, void *user)
{
    if (!s_client || !json || !cb || !s_req_mtx) return ESP_FAIL;
    int slot = -1;
    xSemaphoreTake(s_req_mtx, portMAX_DELAY);
    for (int i = 0; i < HA_WS_REQ_SLOTS; i++) {
        if (s_req[i].id == 0) {
            s_req[i].id = id; s_req[i].cb = cb; s_req[i].user = user;
            slot = i; break;
        }
    }
    xSemaphoreGive(s_req_mtx);
    if (slot < 0) {
        ESP_LOGW(TAG, "ha_ws_request: aucun slot libre (id=%d)", id);
        return ESP_FAIL;
    }
    if (ha_ws_send_cmd(json) != ESP_OK) {
        xSemaphoreTake(s_req_mtx, portMAX_DELAY);
        s_req[slot].id = 0;   /* libère le slot : pas de réponse attendue */
        xSemaphoreGive(s_req_mtx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool req_dispatch(int id, cJSON *root)
{
    ha_ws_result_cb_t cb = NULL;
    void *user = NULL;
    if (!s_req_mtx) return false;
    xSemaphoreTake(s_req_mtx, portMAX_DELAY);
    for (int i = 0; i < HA_WS_REQ_SLOTS; i++) {
        if (s_req[i].id == id) {
            cb = s_req[i].cb; user = s_req[i].user;
            s_req[i].id = 0;   /* one-shot */
            break;
        }
    }
    xSemaphoreGive(s_req_mtx);
    if (cb) { cb(root, user); return true; }
    return false;
}

/* Réponse à auth_required : on présente le token longue durée. */
static void send_auth(void)
{
    /* Le token est secret : le buffer n'est jamais loggué. */
    char msg[700];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"auth\",\"access_token\":\"%s\"}", app_config_ha_token());
    ws_send(msg);
    ESP_LOGI(TAG, "→ auth (token présenté)");
}

static void send_subscribe(void)
{
    char msg[96];
    snprintf(msg, sizeof(msg),
             "{\"id\":%d,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}",
             HA_WS_SUB_ID);
    ws_send(msg);
    ESP_LOGI(TAG, "→ subscribe_events(state_changed) id=%d", HA_WS_SUB_ID);
}

/* Traite un message JSON complet venant de HA. */
static void handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON invalide (%d octets)", len);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "auth_required") == 0) {
        s_auth_invalid = false;
        send_auth();
    } else if (strcmp(t, "auth_ok") == 0) {
        s_authed = true;
        s_auth_invalid = false;
        ESP_LOGI(TAG, "← auth_ok");
        send_subscribe();
    } else if (strcmp(t, "auth_invalid") == 0) {
        s_auth_invalid = true;
        const cJSON *m = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "← auth_invalid: %s",
                 cJSON_IsString(m) ? m->valuestring : "?");
    } else if (strcmp(t, "result") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        const cJSON *ok = cJSON_GetObjectItem(root, "success");
        int idv = cJSON_IsNumber(id) ? id->valueint : -1;
        if (idv == HA_WS_SUB_ID) {
            s_subscribed = cJSON_IsTrue(ok);
            ESP_LOGI(TAG, "← result abonnement états: %s",
                     s_subscribed ? "OK" : "ÉCHEC");
        } else {
            req_dispatch(idv, root);
        }
    } else if (strcmp(t, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        cJSON *data_o = event ? cJSON_GetObjectItem(event, "data") : NULL;
        cJSON *eid = data_o ? cJSON_GetObjectItem(data_o, "entity_id") : NULL;
        cJSON *ns  = data_o ? cJSON_GetObjectItem(data_o, "new_state") : NULL;
        if (cJSON_IsString(eid) && ns && ha_worker_tracks(eid->valuestring)) {
            char *js = cJSON_PrintUnformatted(ns);   /* new_state sérialisé */
            if (js) {
                s_events_received++;
                /* le worker prend possession de js et le libère après dispatch */
                ha_worker_post_state_event(eid->valuestring, js);
            }
        }
    } else {
        ESP_LOGD(TAG, "← message type=%s", t);
    }
    cJSON_Delete(root);
}

static void on_ws_event(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connecté — attente de auth_required");
        s_authed = s_subscribed = false;
        s_rx_len = 0;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "déconnecté (reconnexion auto)");
        s_authed = s_subscribed = false;
        s_rx_len = 0;
        s_reconnects++;
        break;

    case WEBSOCKET_EVENT_DATA:
        /* op_code 0x01 = texte, 0x00 = continuation. On ignore ping/pong/close. */
        if (d->op_code != 0x01 && d->op_code != 0x00) break;
        if (d->payload_len <= 0) break;
        if (d->payload_offset == 0) s_rx_len = 0;   /* début d'une nouvelle frame logique */
        if (s_rx && d->data_len > 0) {
            int space = HA_WS_RX_CAP - 1 - s_rx_len;
            int n = (d->data_len < space) ? d->data_len : space;
            if (n > 0) {
                memcpy(s_rx + s_rx_len, d->data_ptr, n);
                s_rx_len += n;
                s_rx[s_rx_len] = '\0';
            }
            if (d->data_len > space) {
                /* Frame trop grosse : tronquée → le parse échouera et l'event
                 * sera ignoré. Pas de corruption ; rattrapé par le poll de secours. */
                s_frames_truncated++;
                ESP_LOGW(TAG, "frame tronquée (>%d octets) — event ignoré", HA_WS_RX_CAP);
            }
        }
        if (d->payload_offset + d->data_len >= d->payload_len && s_rx_len > 0) {
            handle_message(s_rx, s_rx_len);
            s_rx_len = 0;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "erreur WS");
        break;

    default:
        break;
    }
}

esp_err_t ha_ws_start(void)
{
    if (s_client) return ESP_OK;   /* déjà démarré */

    /* Compte HA non configuré (URL/token vides) → ne pas démarrer le client :
     * une URI « ws:///… » ferait échouer/boucler la connexion. L'utilisateur
     * configure HA via le portail web, ce qui redémarre le panneau. */
    if (!app_config_ha_set()) {
        ESP_LOGW(TAG, "Compte HA non configuré — WebSocket en attente (portail)");
        return ESP_OK;
    }

    s_rx = malloc(HA_WS_RX_CAP);   /* heap interne : pas de PSRAM sur ce SoC */
    if (!s_rx) return ESP_ERR_NO_MEM;

    if (!s_req_mtx) s_req_mtx = xSemaphoreCreateMutex();

    char uri[128];
    build_ws_uri(uri, sizeof(uri));
    ESP_LOGI(TAG, "URI %s", uri);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 4096,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = false,
        /* Détection rapide d'un socket half-open : PING 10 s, abandon si pas de
         * PONG en 15 s → reconnexion auto (défaut = 120 s, bien trop lent ici). */
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 15,
        .keep_alive_enable = true,
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        free(s_rx); s_rx = NULL;
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "démarrage WS échoué: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "client WS démarré");
    return ESP_OK;
}

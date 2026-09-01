/*
 * Client Home Assistant (REST). S'appuie sur esp_http_client pour les requêtes
 * et cJSON pour le décodage. Authentification par token longue durée (Bearer).
 *
 * Allégé de ha-panel/main/ha/ha_client.c (1657 l. → ce fichier). Contrairement
 * à l'original, ha_client_init() n'existe pas ici : l'ESP32-D0WD n'a pas de
 * PSRAM, cJSON utilise donc l'allocateur par défaut (les hooks SPIRAM de
 * l'original auraient rendu NULL pour chaque nœud). set_auth_headers() écrit
 * dans un buffer de pile (pas static) : deux tâches (ha_worker, rooms_fetch)
 * appellent maintenant l'API REST en parallèle.
 */
#include "ha_client.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "app_config.h"

static const char *TAG = "ha";

#define HA_RESP_MAX_LEN   2048   /* un état de lumière/climate JSON tient largement */

/* Accumulateur de réponse HTTP */
typedef struct {
    char  *buf;
    int    len;
    int    cap;
} resp_acc_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        resp_acc_t *acc = (resp_acc_t *)evt->user_data;
        int space = acc->cap - acc->len - 1;
        int n = (evt->data_len < space) ? evt->data_len : space;
        if (n > 0) {
            memcpy(acc->buf + acc->len, evt->data, n);
            acc->len += n;
            acc->buf[acc->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Construit "<base>/api/...". Le caller fournit le chemin complet après /api. */
static void build_url(char *out, size_t out_len, const char *path)
{
    snprintf(out, out_len, "%s%s", app_config_ha_url(), path);
}

static void set_auth_headers(esp_http_client_handle_t client)
{
    char auth[600];   /* pile, pas static : ha_worker et rooms_fetch appellent en parallèle */
    snprintf(auth, sizeof(auth), "Bearer %s", app_config_ha_token());
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
}

/* Cœur HTTP HA : init + auth + (POST body) + perform + statut + cleanup.
 *   path     : chemin après la base (ex "/api/states/x").
 *   post     : corps JSON pour un POST, NULL pour un GET.
 *   resp/cap : buffer d'accumulation de la réponse (NULL/0 si le corps est ignoré).
 * Renvoie ESP_OK seulement si le transport aboutit ET le statut est 200/201. */
static esp_err_t ha_http_request(esp_http_client_method_t method, const char *path,
                                 const char *post, char *resp, int cap, int timeout_ms)
{
    char url[256];
    build_url(url, sizeof(url), path);
    resp_acc_t acc = { .buf = resp, .len = 0, .cap = cap };
    if (resp && cap > 0) resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = resp ? http_event_cb : NULL,
        .user_data = resp ? &acc : NULL,
        .timeout_ms = timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    set_auth_headers(client);
    if (post) esp_http_client_set_post_field(client, post, strlen(post));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", path, esp_err_to_name(err));
        return err;
    }
    if (status != 200 && status != 201) {
        ESP_LOGE(TAG, "%s → HTTP %d", path, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* GET path puis parse la réponse JSON. *out = arbre cJSON (à cJSON_Delete par
 * l'appelant) ; NULL et ESP_FAIL si la requête ou le parse échoue. */
static esp_err_t ha_http_get_json(const char *path, int cap, cJSON **out)
{
    *out = NULL;
    char *body = calloc(1, cap);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t err = ha_http_request(HTTP_METHOD_GET, path, NULL, body, cap, 5000);
    if (err == ESP_OK) {
        *out = cJSON_Parse(body);
        if (!*out) { ESP_LOGE(TAG, "%s: JSON invalide", path); err = ESP_FAIL; }
    }
    free(body);
    return err;
}

/* ---- Lumières ---- */

esp_err_t ha_parse_light_state(cJSON *root, bool *is_on)
{
    if (!root || !is_on) return ESP_ERR_INVALID_ARG;
    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state) && state->valuestring) {
        *is_on = (strcmp(state->valuestring, "on") == 0);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t ha_parse_light(cJSON *root, ha_light_state_t *out)
{
    if (!root || !out) return ESP_ERR_INVALID_ARG;
    out->on = false; out->dimmable = false; out->brightness = 0;
    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state) || !state->valuestring) return ESP_FAIL;
    out->on = (strcmp(state->valuestring, "on") == 0);
    cJSON *a = cJSON_GetObjectItem(root, "attributes");
    if (a) {
        cJSON *br = cJSON_GetObjectItem(a, "brightness");
        if (cJSON_IsNumber(br)) out->brightness = br->valueint;   /* 0..255 */
        /* Dimmable si supported_color_modes contient un mode autre que "onoff"
         * (absent sur un switch.* : reste false, ha_light_set_brightness à éviter). */
        cJSON *scm = cJSON_GetObjectItem(a, "supported_color_modes");
        if (cJSON_IsArray(scm)) {
            cJSON *m;
            cJSON_ArrayForEach(m, scm) {
                if (cJSON_IsString(m) && m->valuestring && strcmp(m->valuestring, "onoff") != 0) {
                    out->dimmable = true;
                    break;
                }
            }
        }
    }
    return ESP_OK;
}

esp_err_t ha_get_light(const char *entity_id, ha_light_state_t *out)
{
    if (!entity_id || !out) return ESP_ERR_INVALID_ARG;

    char path[160];
    snprintf(path, sizeof(path), "/api/states/%s", entity_id);
    cJSON *root;   /* cap 4096 : attributs (color modes, effets…) plus gros qu'un on/off */
    esp_err_t err = ha_http_get_json(path, 4096, &root);
    if (err != ESP_OK) return err;

    esp_err_t ret = ha_parse_light(root, out);
    cJSON_Delete(root);
    return ret;
}

/* Appelle un service homeassistant/<service> sur une entité (toggle/turn_on/...).
 * Domaine générique homeassistant.* : fonctionne aussi bien sur light.* que sur
 * switch.* (cas des lumières labellisées qui sont en réalité des prises). */
static esp_err_t call_service(const char *service, const char *entity_id)
{
    if (!entity_id) return ESP_ERR_INVALID_ARG;

    char path[80], post[160];
    snprintf(path, sizeof(path), "/api/services/homeassistant/%s", service);
    snprintf(post, sizeof(post), "{\"entity_id\":\"%s\"}", entity_id);

    esp_err_t err = ha_http_request(HTTP_METHOD_POST, path, post, NULL, 0, 5000);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s %s", service, entity_id);
    return err;
}

esp_err_t ha_light_toggle(const char *entity_id)
{
    return call_service("toggle", entity_id);
}

esp_err_t ha_light_set(const char *entity_id, bool on)
{
    return call_service(on ? "turn_on" : "turn_off", entity_id);
}

esp_err_t ha_light_set_brightness(const char *entity_id, int pct)
{
    if (!entity_id) return ESP_ERR_INVALID_ARG;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", entity_id, pct);
    return ha_http_request(HTTP_METHOD_POST, "/api/services/light/turn_on", body, NULL, 0, 5000);
}

/* ---- Scénarios (scenes) / scripts ---- */

static esp_err_t service_post(const char *domain, const char *service, const char *json_body)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
    return ha_http_request(HTTP_METHOD_POST, path, json_body, NULL, 0, 5000);
}

esp_err_t ha_scene_activate(const char *entity_id)
{
    if (!entity_id) return ESP_ERR_INVALID_ARG;
    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
    return service_post("scene", "turn_on", body);
}

esp_err_t ha_script_run(const char *entity_id)
{
    if (!entity_id) return ESP_ERR_INVALID_ARG;
    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
    return service_post("script", "turn_on", body);
}

/* ---- Templates ---- */

esp_err_t ha_render_template(const char *tmpl, char *out, size_t out_len)
{
    if (!tmpl || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    /* Corps JSON {"template": "..."} construit via cJSON (échappe le template). */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "template", tmpl);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    /* Réponse = texte rendu (pas du JSON) → accumulé directement dans `out`. */
    esp_err_t err = ha_http_request(HTTP_METHOD_POST, "/api/template", body,
                                    out, (int)out_len, 8000);
    free(body);
    return err;
}

/* ---- Chauffage (climate) — posé pour la phase 3 ---- */

static void json_copy_str(cJSON *obj, const char *key, char *dst, size_t n)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(dst, it->valuestring, n - 1);
        dst[n - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static float json_num(cJSON *obj, const char *key, float def)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : def;
}

esp_err_t ha_parse_climate_state(cJSON *root, ha_climate_state_t *out)
{
    if (!root || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->min_temp = 7;
    out->max_temp = 30;
    out->step = 0.5f;

    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state)) strncpy(out->state, state->valuestring, sizeof(out->state) - 1);
    cJSON *a = cJSON_GetObjectItem(root, "attributes");
    if (a) {
        out->current_temp = json_num(a, "current_temperature", 0);
        out->target_temp  = json_num(a, "temperature", 0);
        out->min_temp     = json_num(a, "min_temp", 7);
        out->max_temp     = json_num(a, "max_temp", 30);
        out->step         = json_num(a, "target_temp_step", 0.5f);
        json_copy_str(a, "hvac_action", out->action, sizeof(out->action));
        json_copy_str(a, "preset_mode", out->preset, sizeof(out->preset));
    }
    out->ok = true;
    return ESP_OK;
}

esp_err_t ha_get_climate_state(const char *entity_id, ha_climate_state_t *out)
{
    if (!entity_id || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->min_temp = 7;
    out->max_temp = 30;
    out->step = 0.5f;

    char path[160];
    snprintf(path, sizeof(path), "/api/states/%s", entity_id);
    cJSON *root;
    esp_err_t err = ha_http_get_json(path, HA_RESP_MAX_LEN, &root);
    if (err != ESP_OK) return err;

    esp_err_t ret = ha_parse_climate_state(root, out);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "climate %s = %s/%s cur=%.1f set=%.1f", entity_id, out->state, out->preset,
                 out->current_temp, out->target_temp);
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t ha_climate_set_temp(const char *entity_id, float temp)
{
    if (!entity_id) return ESP_ERR_INVALID_ARG;
    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"temperature\":%.1f}", entity_id, temp);
    return service_post("climate", "set_temperature", body);
}

esp_err_t ha_climate_set_preset(const char *entity_id, const char *preset)
{
    if (!entity_id || !preset) return ESP_ERR_INVALID_ARG;
    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"preset_mode\":\"%s\"}", entity_id, preset);
    return service_post("climate", "set_preset_mode", body);
}

/* ---- Météo ---- */

esp_err_t ha_get_weather_pressure(const char *entity_id, float *pressure)
{
    if (!entity_id || !pressure) return ESP_ERR_INVALID_ARG;
    *pressure = NAN;

    char path[160];
    snprintf(path, sizeof(path), "/api/states/%s", entity_id);
    cJSON *root;
    esp_err_t err = ha_http_get_json(path, HA_RESP_MAX_LEN, &root);
    if (err != ESP_OK) return err;

    cJSON *a = cJSON_GetObjectItem(root, "attributes");
    if (a) *pressure = json_num(a, "pressure", NAN);
    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * Worker HA — allégé de ha-panel/main/ui/ha_worker.c (720 l. → ce fichier).
 * Une task : poll de secours des lumières et entités climate suivies (routage
 * par préfixe d'entity_id) + traitement des commandes (toggle/set/luminosité)
 * + dispatch des push WebSocket. Jamais d'accès UART.
 */
#include "ha_worker.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "cJSON.h"

#include "wifi.h"
#include "ha_client.h"

/* 8 pièces (ROOMS_MAX_ROOMS) x 1 climate + 16 lumières (s_glights). */
#define HA_WORKER_MAX_SUBS 24
#define HA_ENTITY_MAXLEN   80

static const char *TAG = "ha_worker";

static char              s_subs[HA_WORKER_MAX_SUBS][HA_ENTITY_MAXLEN];
static int                s_sub_count;
static SemaphoreHandle_t  s_subs_mtx;
static QueueHandle_t      s_cmd_q;
static volatile bool      s_poll_paused;
static esp_timer_handle_t s_settle_timer;   /* re-sonde différée après une commande */

static ha_light_state_cb_t s_light_cb;
static void                *s_light_user;
static ha_climate_state_cb_t s_climate_cb;
static void                  *s_climate_user;

static bool is_climate(const char *entity_id)
{
    return strncmp(entity_id, "climate.", 8) == 0;
}

/* kind : 0 = action lumière (toggle/turn_on/turn_off), 7 = event poussé (WS),
 *        8 = sync immédiate, 12 = luminosité. */
typedef struct {
    char    entity[HA_ENTITY_MAXLEN];
    uint8_t kind;
    uint8_t action;   /* kind 0 : 0=toggle, 1=on, 2=off */
    float   value;    /* kind 12 : pourcentage */
    char   *json;     /* kind 7 : new_state sérialisé, possédé par le worker */
} cmd_t;

static void post_sync(void)
{
    if (!s_cmd_q) return;
    cmd_t c = { .kind = 8 };
    xQueueSend(s_cmd_q, &c, 0);
}

static void settle_timer_cb(void *arg)
{
    (void)arg;
    post_sync();
}

void ha_worker_set_tracked(const char *const *entity_ids, int count)
{
    if (count > HA_WORKER_MAX_SUBS) count = HA_WORKER_MAX_SUBS;
    if (count < 0) count = 0;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    s_sub_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(s_subs[i], entity_ids[i], HA_ENTITY_MAXLEN - 1);
        s_subs[i][HA_ENTITY_MAXLEN - 1] = '\0';
    }
    xSemaphoreGive(s_subs_mtx);
    if (count > 0) post_sync();
}

void ha_worker_set_light_cb(ha_light_state_cb_t cb, void *user)
{
    s_light_cb = cb;
    s_light_user = user;
}

void ha_worker_set_climate_cb(ha_climate_state_cb_t cb, void *user)
{
    s_climate_cb = cb;
    s_climate_user = user;
}

void ha_worker_pause_poll(bool pause) { s_poll_paused = pause; }

static void enqueue(const cmd_t *c)
{
    if (s_cmd_q) xQueueSend(s_cmd_q, c, 0);
}

void ha_worker_request_toggle(const char *entity_id)
{
    if (!entity_id) return;
    cmd_t c = { .kind = 0, .action = 0 };
    strncpy(c.entity, entity_id, HA_ENTITY_MAXLEN - 1);
    enqueue(&c);
}

void ha_worker_request_set(const char *entity_id, bool on)
{
    if (!entity_id) return;
    cmd_t c = { .kind = 0, .action = on ? 1 : 2 };
    strncpy(c.entity, entity_id, HA_ENTITY_MAXLEN - 1);
    enqueue(&c);
}

void ha_worker_request_brightness(const char *entity_id, int pct)
{
    if (!entity_id) return;
    cmd_t c = { .kind = 12, .value = (float)pct };
    strncpy(c.entity, entity_id, HA_ENTITY_MAXLEN - 1);
    enqueue(&c);
}

bool ha_worker_tracks(const char *entity_id)
{
    if (!entity_id || !s_subs_mtx) return false;
    bool found = false;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    for (int i = 0; i < s_sub_count && !found; i++) {
        if (strcmp(s_subs[i], entity_id) == 0) found = true;
    }
    xSemaphoreGive(s_subs_mtx);
    return found;
}

void ha_worker_post_state_event(const char *entity_id, char *json)
{
    if (!entity_id || !json) { free(json); return; }
    cmd_t c = { .kind = 7, .json = json };
    strncpy(c.entity, entity_id, HA_ENTITY_MAXLEN - 1);
    if (!s_cmd_q || xQueueSend(s_cmd_q, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "file pleine — event poussé %s perdu (rattrapé au poll)", entity_id);
        free(json);
    }
}

static void ws_dispatch(const char *entity, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    if (is_climate(entity)) {
        ha_climate_state_t cs;
        if (ha_parse_climate_state(root, &cs) == ESP_OK && s_climate_cb) {
            s_climate_cb(entity, &cs, s_climate_user);
        }
    } else {
        ha_light_state_t ls;
        if (ha_parse_light(root, &ls) == ESP_OK && s_light_cb) {
            s_light_cb(entity, ls.on, s_light_user);
        }
    }
    cJSON_Delete(root);
}

static void ha_worker_task(void *arg)
{
    (void)arg;

    /* Attendre la 1re connexion sans jamais abandonner : le WiFi retente en
     * arrière-plan. S'auto-supprimer sur un FAIL transitoire tuerait le poll
     * ET le dispatch des push WS (queue jamais vidée). */
    while (!wifi_is_connected()) vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "WiFi up, poll toutes les %d ms", CONFIG_HA_POLL_INTERVAL_MS);

    for (;;) {
        cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(CONFIG_HA_POLL_INTERVAL_MS))) {
            if (cmd.kind == 7) {
                ws_dispatch(cmd.entity, cmd.json);
                free(cmd.json);
                continue;
            }
            if (cmd.kind != 8) {
                if (cmd.kind == 12) {
                    ha_light_set_brightness(cmd.entity, (int)cmd.value);
                } else if (cmd.action == 0) {
                    ha_light_toggle(cmd.entity);
                } else {
                    ha_light_set(cmd.entity, cmd.action == 1);
                }
                /* Laisser HA/le device refléter le nouvel état avant de re-sonder ;
                 * un timer one-shot (re)déclenche post_sync() 1200 ms plus tard sans
                 * bloquer la task (les push WS en attente ne s'accumulent pas). */
                if (s_settle_timer) {
                    esp_timer_stop(s_settle_timer);
                    esp_timer_start_once(s_settle_timer, 1200 * 1000);
                }
                continue;
            }
            /* kind 8 (sync) : tombe direct dans le poll ci-dessous. */
        }

        if (s_poll_paused) continue;   /* veille : skip poll réseau */

        char local[HA_WORKER_MAX_SUBS][HA_ENTITY_MAXLEN];
        int n;
        xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
        n = s_sub_count;
        memcpy(local, s_subs, sizeof(char) * HA_ENTITY_MAXLEN * n);
        xSemaphoreGive(s_subs_mtx);

        for (int i = 0; i < n; i++) {
            if (is_climate(local[i])) {
                ha_climate_state_t cs;
                if (ha_get_climate_state(local[i], &cs) == ESP_OK && s_climate_cb) {
                    s_climate_cb(local[i], &cs, s_climate_user);
                }
            } else {
                ha_light_state_t ls;
                if (ha_get_light(local[i], &ls) == ESP_OK && s_light_cb) {
                    s_light_cb(local[i], ls.on, s_light_user);
                }
            }
        }
    }
}

esp_err_t ha_worker_start(void)
{
    s_subs_mtx = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(cmd_t));
    if (!s_subs_mtx || !s_cmd_q) return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t settle_args = { .callback = settle_timer_cb, .name = "ha_settle" };
    if (esp_timer_create(&settle_args, &s_settle_timer) != ESP_OK) {
        ESP_LOGW(TAG, "timer settle indisponible — re-sonde immédiate après commande");
        s_settle_timer = NULL;
    }

    /* Pile en RAM interne (pas de PSRAM sur ce SoC) : pas de décodage JPEG ni
     * de pochette ici, 6 Ko suffisent largement (vs 12 Ko + PSRAM sur le P4). */
    if (xTaskCreate(ha_worker_task, "ha_worker", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "worker démarré");
    return ESP_OK;
}

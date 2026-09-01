/*
 * Modèle de pièces — allégé de ha-panel/main/ui/home_ui.c:443-593, sans LVGL.
 * Voir rooms.h et design/FORK-PLAN.md.
 */
#include "rooms.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "wifi.h"
#include "ha_client.h"
#include "ha_ws.h"
#include "ha_worker.h"

static const char *TAG = "rooms";

typedef struct { char name[48]; char entity[80]; } rlight_t;
typedef struct { char name[48]; rlight_t lights[ROOMS_MAX_LIGHTS]; int count; char climate[80]; } droom_t;
/* Cache plat de toutes les lumières connues (compteur d'accueil + subs worker). */
typedef struct { int room; char entity[80]; bool is_on; } glight_t;

static droom_t  s_rooms[ROOMS_MAX_ROOMS];
static int      s_room_count;
static int      s_room_current;
static glight_t s_glights[16];
static int      s_glight_count;
static volatile bool s_dirty;
static volatile bool s_fetching;

/* Toutes les aires ayant au moins une lumière étiquetée « Light ha panel »
 * (n'importe quel domaine : sur cette installation, 3 des 8 entités du label
 * sont des switch.*). Filtrer les aires vides évite de déborder ROOMS_MAX_ROOMS
 * avec les aires sans lumière (13 aires au total, 6-7 en portent une). */
static const char *ROOMS_TEMPLATE =
    "{% set lb = label_entities('Light ha panel') %}"
    "[{% for a in areas() if area_entities(a) | select('in', lb) | list %}"
    "{\"name\":{{ area_name(a) | tojson }},"
    "\"climate\":{{ ((area_entities(a) | select('match','^climate\\.') | first) | default('')) | tojson }},"
    "\"lights\":["
    "{% for e in area_entities(a) if e in lb %}"
    "{\"id\":{{ e | tojson }},\"name\":{{ (state_attr(e,'friendly_name') or e) | tojson }}}"
    "{% if not loop.last %},{% endif %}{% endfor %}]}"
    "{% if not loop.last %},{% endif %}{% endfor %}]";

static void build_glights(void)
{
    s_glight_count = 0;
    for (int r = 0; r < s_room_count; r++) {
        for (int k = 0; k < s_rooms[r].count && s_glight_count < 16; k++) {
            s_glights[s_glight_count].room = r;
            strncpy(s_glights[s_glight_count].entity, s_rooms[r].lights[k].entity, 79);
            s_glights[s_glight_count].entity[79] = 0;
            s_glights[s_glight_count].is_on = false;
            s_glight_count++;
        }
    }
}

static void apply_rooms_json(const char *json)
{
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "réponse HA non-JSON ou non-tableau, modèle inchangé");
        cJSON_Delete(arr);
        return;
    }
    int rc = 0;
    cJSON *room;
    cJSON_ArrayForEach(room, arr) {
        if (rc >= ROOMS_MAX_ROOMS) break;
        cJSON *name = cJSON_GetObjectItem(room, "name");
        cJSON *lights = cJSON_GetObjectItem(room, "lights");
        if (!cJSON_IsString(name)) continue;
        strncpy(s_rooms[rc].name, name->valuestring, sizeof(s_rooms[rc].name) - 1);
        s_rooms[rc].name[sizeof(s_rooms[rc].name) - 1] = 0;
        cJSON *clim = cJSON_GetObjectItem(room, "climate");
        s_rooms[rc].climate[0] = 0;
        if (cJSON_IsString(clim) && clim->valuestring[0])
            strncpy(s_rooms[rc].climate, clim->valuestring, sizeof(s_rooms[rc].climate) - 1);
        int lc = 0;
        cJSON *l;
        cJSON_ArrayForEach(l, lights) {
            if (lc >= ROOMS_MAX_LIGHTS) break;
            cJSON *id = cJSON_GetObjectItem(l, "id");
            cJSON *ln = cJSON_GetObjectItem(l, "name");
            if (!cJSON_IsString(id)) continue;
            strncpy(s_rooms[rc].lights[lc].entity, id->valuestring, 79);
            s_rooms[rc].lights[lc].entity[79] = 0;
            const char *nm = cJSON_IsString(ln) ? ln->valuestring : id->valuestring;
            strncpy(s_rooms[rc].lights[lc].name, nm, sizeof(s_rooms[rc].lights[lc].name) - 1);
            s_rooms[rc].lights[lc].name[sizeof(s_rooms[rc].lights[lc].name) - 1] = 0;
            lc++;
        }
        s_rooms[rc].count = lc;
        rc++;
    }
    cJSON_Delete(arr);

    if (rc > 0) {
        s_room_count = rc;
    } else {
        s_room_count = 1;
        strcpy(s_rooms[0].name, "Aucune pièce");
        s_rooms[0].count = 0;
        s_rooms[0].climate[0] = 0;
    }
    if (s_room_current >= s_room_count) s_room_current = 0;
    build_glights();
    s_dirty = true;

    /* Réabonne le worker sur la liste à jour des lumières (WS + poll de secours). */
    const char *ids[16];
    for (int i = 0; i < s_glight_count; i++) ids[i] = s_glights[i].entity;
    ha_worker_set_tracked(ids, s_glight_count);

    ESP_LOGI(TAG, "modèle pièces: %d pièce(s), %d lumière(s)", s_room_count, s_glight_count);
}

static void on_light_state(const char *entity_id, bool on, void *user)
{
    (void)user;
    for (int i = 0; i < s_glight_count; i++) {
        if (strcmp(s_glights[i].entity, entity_id) == 0) {
            if (s_glights[i].is_on != on) {
                s_glights[i].is_on = on;
                s_dirty = true;
            }
            break;
        }
    }
}

static void rooms_fetch_task(void *arg)
{
    (void)arg;
    if (!wifi_wait_connected(0)) {
        s_fetching = false;
        vTaskDelete(NULL);
        return;
    }
    char *buf = malloc(4096);
    if (buf) {
        if (ha_render_template(ROOMS_TEMPLATE, buf, 4096) == ESP_OK) {
            apply_rooms_json(buf);
        } else {
            ESP_LOGW(TAG, "ha_render_template a échoué, modèle inchangé");
        }
        free(buf);
    }
    s_fetching = false;
    vTaskDelete(NULL);
}

void rooms_start_fetch(void)
{
    if (s_fetching) return;
    s_fetching = true;
    if (xTaskCreate(rooms_fetch_task, "rooms_fetch", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rooms_fetch: création tâche échouée (RAM)");
        s_fetching = false;
    }
}

bool rooms_fetching(void) { return s_fetching; }

int rooms_count(void) { return s_room_count; }

const char *rooms_name(int i)
{
    return (i >= 0 && i < s_room_count) ? s_rooms[i].name : "";
}

int rooms_light_count(int i)
{
    return (i >= 0 && i < s_room_count) ? s_rooms[i].count : 0;
}

const char *rooms_climate_entity(int i)
{
    return (i >= 0 && i < s_room_count) ? s_rooms[i].climate : "";
}

int rooms_current(void) { return s_room_current; }

void rooms_set_current(int i)
{
    if (i >= 0 && i < s_room_count) { s_room_current = i; s_dirty = true; }
}

int rooms_lights_total(void) { return s_glight_count; }

int rooms_lights_on(void)
{
    int n = 0;
    for (int i = 0; i < s_glight_count; i++) if (s_glights[i].is_on) n++;
    return n;
}

bool rooms_model_dirty(void)
{
    if (!s_dirty) return false;
    s_dirty = false;
    return true;
}

/* Relance périodique du modèle (structure des pièces ~statique mais pas figée :
 * un renommage d'aire côté HA doit finir par se voir sans reboot), et à la
 * reconnexion WebSocket (une coupure prolongée peut avoir manqué des
 * changements). Tick 30 s : ~30 min pour la relance périodique. */
#define ROOMS_TICK_MS        30000
#define ROOMS_PERIODIC_TICKS 60

static void rooms_watchdog_task(void *arg)
{
    (void)arg;
    bool was_ready = false;
    int  ticks = 0;

    ha_worker_set_light_cb(on_light_state, NULL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ROOMS_TICK_MS));
        bool ready = ha_ws_is_ready();
        if (ready && !was_ready) {
            ESP_LOGI(TAG, "WS (re)connecté — relance du fetch pièces");
            rooms_start_fetch();
            ticks = 0;
        }
        was_ready = ready;
        if (++ticks >= ROOMS_PERIODIC_TICKS) {
            rooms_start_fetch();
            ticks = 0;
        }
    }
}

esp_err_t rooms_watchdog_start(void)
{
    return xTaskCreate(rooms_watchdog_task, "rooms_wd", 3072, NULL, 3, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

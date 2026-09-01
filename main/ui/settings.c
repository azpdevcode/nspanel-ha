/*
 * Réglages persistés — allégé de ha-panel/main/ui/home_ui.c:318-432.
 * Namespace NVS partagé avec app_config.c (voir app_config.c:14).
 */
#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#include "nextion.h"

static const char *TAG = "settings";

#define NS  "nspanel"

static char s_default_room[48];
static int  s_brightness_pct = 80;
static int  s_sleep_enabled  = 1;
static int  s_sleep_timeout_sec = 60;

void settings_load(void)
{
    nvs_handle_t h = 0;
    nvs_open(NS, NVS_READONLY, &h);   /* échoue si le namespace n'existe pas encore */

    size_t len = sizeof(s_default_room);
    if (!h || nvs_get_str(h, "def_room", s_default_room, &len) != ESP_OK)
        s_default_room[0] = '\0';

    int32_t v;
    if (h && nvs_get_i32(h, "brightness", &v) == ESP_OK) s_brightness_pct = v;
    if (h && nvs_get_i32(h, "sleep_en", &v) == ESP_OK) s_sleep_enabled = v;
    if (h && nvs_get_i32(h, "sleep_sec", &v) == ESP_OK) s_sleep_timeout_sec = v;

    if (h) nvs_close(h);

    /* Effets de bord matériels : la veille est déléguée à l'écran (thsp/thup),
     * cf. design/FORK-PLAN.md section « Veille ». Page-agnostiques : toujours
     * tentés, no-op silencieux si l'écran n'a pas encore répondu. */
    nx_dim((uint8_t)s_brightness_pct);
    if (s_sleep_enabled) nx_cmd("thsp=%d", s_sleep_timeout_sec);
    else nx_cmd("thsp=0");
    nx_cmd("thup=1");

    ESP_LOGI(TAG, "réglages: pièce='%s' luminosité=%d%% veille=%s(%ds)",
             s_default_room, s_brightness_pct,
             s_sleep_enabled ? "on" : "off", s_sleep_timeout_sec);
}

const char *settings_default_room(void) { return s_default_room; }

static esp_err_t save_i32(const char *key, int32_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return err; }
    err = nvs_set_i32(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_save_default_room(const char *name)
{
    if (!name) name = "";
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "def_room", name) == ESP_OK && nvs_commit(h) == ESP_OK) {
        strncpy(s_default_room, name, sizeof(s_default_room) - 1);
        s_default_room[sizeof(s_default_room) - 1] = '\0';
    }
    nvs_close(h);
}

int settings_brightness(void) { return s_brightness_pct; }

void settings_save_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (save_i32("brightness", pct) == ESP_OK) {
        s_brightness_pct = pct;
        nx_dim((uint8_t)pct);
    }
}

int settings_sleep_enabled(void) { return s_sleep_enabled; }

void settings_save_sleep_enabled(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (save_i32("sleep_en", enabled) == ESP_OK) {
        s_sleep_enabled = enabled;
        nx_cmd("thsp=%d", enabled ? s_sleep_timeout_sec : 0);
    }
}

int settings_sleep_timeout_s(void) { return s_sleep_timeout_sec; }

void settings_save_sleep_timeout(int sec)
{
    if (sec < 5) sec = 5;
    if (save_i32("sleep_sec", sec) == ESP_OK) {
        s_sleep_timeout_sec = sec;
        if (s_sleep_enabled) nx_cmd("thsp=%d", sec);
    }
}

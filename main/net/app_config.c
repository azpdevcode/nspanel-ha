/*
 * Config runtime (WiFi + compte HA) persistée en NVS. Voir app_config.h.
 */
#include "app_config.h"

#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "app_cfg";

#define NS  "nspanel"   /* namespace partagé avec les réglages UI (ui/settings.c) */

/* Cache RAM (chargé au boot, rafraîchi par les setters). */
static char s_ssid[APP_CFG_SSID_MAX];
static char s_pass[APP_CFG_PASS_MAX];
static char s_url[APP_CFG_URL_MAX];
static char s_token[APP_CFG_TOKEN_MAX];
static char s_mqtt_uri[APP_CFG_MQTT_URI_MAX];
static char s_mqtt_user[APP_CFG_MQTT_USER_MAX];
static char s_mqtt_pass[APP_CFG_MQTT_PASS_MAX];

/* Charge une clé chaîne ; repli sur `fallback` si absente/erreur/trop longue. */
static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap,
                     const char *fallback)
{
    size_t len = cap;
    if (h && nvs_get_str(h, key, out, &len) == ESP_OK) {
        return;
    }
    strncpy(out, fallback, cap - 1);
    out[cap - 1] = '\0';
}

void app_config_init(void)
{
    nvs_handle_t h = 0;
    /* NVS_READONLY échoue si le namespace n'existe pas encore (premier boot) :
     * h reste 0 → load_str() prend les replis Kconfig. */
    nvs_open(NS, NVS_READONLY, &h);

    load_str(h, "wifi_ssid", s_ssid,  sizeof(s_ssid),  CONFIG_WIFI_SSID);
    load_str(h, "wifi_pass", s_pass,  sizeof(s_pass),  CONFIG_WIFI_PASSWORD);
    load_str(h, "ha_url",    s_url,   sizeof(s_url),   CONFIG_HA_BASE_URL);
    load_str(h, "ha_token",  s_token, sizeof(s_token), CONFIG_HA_TOKEN);
    load_str(h, "mqtt_uri",  s_mqtt_uri,  sizeof(s_mqtt_uri),  CONFIG_MQTT_BROKER_URI);
    load_str(h, "mqtt_user", s_mqtt_user, sizeof(s_mqtt_user), CONFIG_MQTT_USERNAME);
    load_str(h, "mqtt_pass", s_mqtt_pass, sizeof(s_mqtt_pass), CONFIG_MQTT_PASSWORD);

    if (h) nvs_close(h);

    /* Ne jamais loguer mot de passe ni token. */
    ESP_LOGI(TAG, "config: ssid='%s' (%s), ha_url='%s' (token %s)",
             s_ssid, app_config_wifi_set() ? "set" : "vide",
             s_url, app_config_ha_set() ? "présent" : "absent");
}

const char *app_config_wifi_ssid(void)  { return s_ssid; }
const char *app_config_wifi_pass(void)  { return s_pass; }
const char *app_config_ha_url(void)     { return s_url; }
const char *app_config_ha_token(void)   { return s_token; }
const char *app_config_mqtt_uri(void)   { return s_mqtt_uri; }
const char *app_config_mqtt_user(void)  { return s_mqtt_user; }
const char *app_config_mqtt_pass(void)  { return s_mqtt_pass; }

bool app_config_wifi_set(void) { return s_ssid[0] != '\0'; }
bool app_config_ha_set(void)   { return s_url[0] != '\0' && s_token[0] != '\0'; }
bool app_config_mqtt_set(void) { return s_mqtt_uri[0] != '\0'; }

static esp_err_t save_two(const char *k1, const char *v1, char *cache1, size_t cap1,
                          const char *k2, const char *v2, char *cache2, size_t cap2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK) err = nvs_set_str(h, k2, v2);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return err;
    }
    /* Cache à jour seulement après commit réussi. */
    strncpy(cache1, v1, cap1 - 1); cache1[cap1 - 1] = '\0';
    strncpy(cache2, v2, cap2 - 1); cache2[cap2 - 1] = '\0';
    return ESP_OK;
}

esp_err_t app_config_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid) ssid = "";
    if (!pass) pass = "";
    ESP_LOGI(TAG, "set wifi ssid='%s'", ssid);
    return save_two("wifi_ssid", ssid, s_ssid, sizeof(s_ssid),
                    "wifi_pass", pass, s_pass, sizeof(s_pass));
}

esp_err_t app_config_set_ha(const char *url, const char *token)
{
    if (!url)   url = "";
    if (!token) token = "";
    ESP_LOGI(TAG, "set ha url='%s' (token %s)", url, token[0] ? "présent" : "absent");
    return save_two("ha_url", url, s_url, sizeof(s_url),
                    "ha_token", token, s_token, sizeof(s_token));
}

esp_err_t app_config_set_mqtt(const char *uri, const char *user, const char *pass)
{
    if (!uri)  uri = "";
    if (!user) user = "";
    if (!pass) pass = "";
    ESP_LOGI(TAG, "set mqtt uri='%s' user='%s'", uri, user);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, "mqtt_uri", uri);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_user", user);
    if (err == ESP_OK) err = nvs_set_str(h, "mqtt_pass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
        return err;
    }

    strncpy(s_mqtt_uri, uri, sizeof(s_mqtt_uri) - 1);   s_mqtt_uri[sizeof(s_mqtt_uri) - 1] = '\0';
    strncpy(s_mqtt_user, user, sizeof(s_mqtt_user) - 1); s_mqtt_user[sizeof(s_mqtt_user) - 1] = '\0';
    strncpy(s_mqtt_pass, pass, sizeof(s_mqtt_pass) - 1); s_mqtt_pass[sizeof(s_mqtt_pass) - 1] = '\0';
    return ESP_OK;
}

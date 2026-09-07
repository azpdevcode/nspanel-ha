#pragma once

/*
 * Configuration runtime du panneau (identifiants WiFi + compte Home Assistant).
 *
 * Source unique de vérité : ces 4 valeurs étaient autrefois figées à la
 * compilation (CONFIG_WIFI_SSID/PASSWORD, CONFIG_HA_BASE_URL/TOKEN). Elles sont
 * désormais lues en NVS au démarrage (namespace « nspanel ») et configurables
 * depuis le panneau (WiFi : scan/saisie sur l'écran ; compte HA : portail web
 * local). Repli sur les valeurs Kconfig si la clé NVS est absente — ces valeurs
 * Kconfig sont vides par défaut, donc le panneau démarre « non configuré ».
 *
 * Les getters renvoient un cache statique chargé une fois par app_config_init()
 * et rafraîchi par les setters → pointeurs stables, utilisables directement dans
 * des snprintf/strncpy (wifi.c, ha_client.c, ha_ws.c).
 */
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tailles de buffer (incluent le terminateur). */
#define APP_CFG_SSID_MAX   33    /* 32 + NUL (limite WiFi) */
#define APP_CFG_PASS_MAX   65    /* 64 + NUL (limite WPA2) */
#define APP_CFG_URL_MAX    128
#define APP_CFG_TOKEN_MAX  600   /* JWT longue durée ~180 c., large marge */
#define APP_CFG_MQTT_URI_MAX  128   /* ex. mqtt://192.168.1.10:1883 */
#define APP_CFG_MQTT_USER_MAX 65
#define APP_CFG_MQTT_PASS_MAX 65

/** Charge la config depuis NVS dans le cache (repli Kconfig). À appeler une fois
 *  au boot, après nvs_flash_init() et avant wifi_start()/ha_*. */
void app_config_init(void);

/* Getters — pointeurs stables vers le cache interne (jamais NULL). */
const char *app_config_wifi_ssid(void);
const char *app_config_wifi_pass(void);
const char *app_config_ha_url(void);
const char *app_config_ha_token(void);
const char *app_config_mqtt_uri(void);
const char *app_config_mqtt_user(void);
const char *app_config_mqtt_pass(void);

/** true si un SSID WiFi non vide est configuré. */
bool app_config_wifi_set(void);
/** true si une URL HA et un token non vides sont configurés. */
bool app_config_ha_set(void);
/** true si une URI de broker MQTT non vide est configurée (user/pass optionnels). */
bool app_config_mqtt_set(void);

/** Enregistre les identifiants WiFi (NVS + cache). */
esp_err_t app_config_set_wifi(const char *ssid, const char *pass);
/** Enregistre le compte HA (NVS + cache). */
esp_err_t app_config_set_ha(const char *url, const char *token);
/** Enregistre le broker MQTT (NVS + cache). user/pass peuvent être vides (broker anonyme). */
esp_err_t app_config_set_mqtt(const char *uri, const char *user, const char *pass);

#ifdef __cplusplus
}
#endif

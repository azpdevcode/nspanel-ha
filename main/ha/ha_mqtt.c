/*
 * Client MQTT vers Home Assistant — voir ha_mqtt.h pour le rôle de ce module
 * dans l'architecture (publication, alors que ha_ws/ha_client ne font que
 * lire/appeler des services).
 *
 * Toute la publication (état, découverte) et la réception de commandes passe
 * par le handler d'événements esp-mqtt, appelé depuis SA PROPRE tâche (créée
 * par esp_mqtt_client_start) : jamais depuis la tâche `nspanel_io`, jamais de
 * blocage réseau depuis io_task. esp_mqtt_client_publish() en QoS 0 est
 * non bloquant et thread-safe, donc aucun verrou n'est nécessaire côté
 * appelant (app_main, nspanel_io) pour les fonctions ha_mqtt_publish_*().
 */
#include "ha_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "app_config.h"
#include "nspanel_io.h"
#include "settings.h"

static const char *TAG = "ha_mqtt";

#define DISCOVERY_PREFIX "homeassistant"
#define TOPIC_MAX  96

static esp_mqtt_client_handle_t s_client;
static char s_node[24];   /* "nspanel_xxxxxx" (6 derniers hexa de la MAC STA) */
static ha_mqtt_stats_t s_stats;

/* ---- Topics ---- */

static void topic(char *out, size_t cap, const char *fmt, ...)
{
    char rel[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof(rel), fmt, ap);
    va_end(ap);
    snprintf(out, cap, "nspanel/%s/%s", s_node, rel);
}

static void publish(const char *t, const char *payload, bool retain)
{
    if (!s_client) return;
    int id = esp_mqtt_client_publish(s_client, t, payload, 0, 0, retain);
    if (id >= 0) s_stats.messages_published++;
}

/* ---- Découverte (config retenue par entité) ---- */

static cJSON *device_block(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    char name[32];
    snprintf(name, sizeof(name), "NSPanel %s", s_node + strlen("nspanel_"));
    cJSON_AddStringToObject(dev, "name", name);
    cJSON_AddStringToObject(dev, "manufacturer", "Sonoff");
    cJSON_AddStringToObject(dev, "model", "NSPanel");
    cJSON_AddStringToObject(dev, "sw_version", app->version);
    return dev;
}

static void publish_discovery_entity(const char *component, const char *object_id,
                                     void (*fill)(cJSON *root, int idx), int idx)
{
    cJSON *root = cJSON_CreateObject();
    char uniq[40];
    snprintf(uniq, sizeof(uniq), "%s_%s", s_node, object_id);
    cJSON_AddStringToObject(root, "unique_id", uniq);
    cJSON_AddItemToObject(root, "device", device_block());
    char avail[TOPIC_MAX];
    topic(avail, sizeof(avail), "status");
    cJSON_AddStringToObject(root, "availability_topic", avail);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    if (fill) fill(root, idx);

    char cfg_topic[TOPIC_MAX];
    snprintf(cfg_topic, sizeof(cfg_topic), DISCOVERY_PREFIX "/%s/%s/%s/config",
             component, s_node, object_id);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body) {
        publish(cfg_topic, body, true);
        free(body);
    }
}

static void fill_relay(cJSON *r, int idx)
{
    char name[24], st[TOPIC_MAX], cmd[TOPIC_MAX];
    snprintf(name, sizeof(name), "Relais %d", idx + 1);
    cJSON_AddStringToObject(r, "name", name);
    topic(st, sizeof(st), "relay/%d/state", idx + 1);
    topic(cmd, sizeof(cmd), "relay/%d/set", idx + 1);
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "command_topic", cmd);
    cJSON_AddStringToObject(r, "payload_on", "ON");
    cJSON_AddStringToObject(r, "payload_off", "OFF");
    cJSON_AddStringToObject(r, "device_class", "switch");
}

static void fill_button_event(cJSON *r, int idx)
{
    char name[24], st[TOPIC_MAX];
    snprintf(name, sizeof(name), "Bouton %d", idx + 1);
    cJSON_AddStringToObject(r, "name", name);
    topic(st, sizeof(st), "button/%d/event", idx + 1);
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON *types = cJSON_CreateArray();
    cJSON_AddItemToArray(types, cJSON_CreateString("press"));
    cJSON_AddItemToArray(types, cJSON_CreateString("long_press"));
    cJSON_AddItemToObject(r, "event_types", types);
    cJSON_AddStringToObject(r, "device_class", "button");
}

static void fill_detach(cJSON *r, int idx)
{
    char name[40], st[TOPIC_MAX], cmd[TOPIC_MAX];
    snprintf(name, sizeof(name), "Bouton %d d\xc3\xa9tach\xc3\xa9", idx + 1);
    cJSON_AddStringToObject(r, "name", name);
    topic(st, sizeof(st), "detach/%d/state", idx + 1);
    topic(cmd, sizeof(cmd), "detach/%d/set", idx + 1);
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "command_topic", cmd);
    cJSON_AddStringToObject(r, "payload_on", "ON");
    cJSON_AddStringToObject(r, "payload_off", "OFF");
    cJSON_AddStringToObject(r, "entity_category", "config");
    cJSON_AddStringToObject(r, "icon", "mdi:link-off");
}

static void fill_temperature(cJSON *r, int idx)
{
    (void)idx;
    char st[TOPIC_MAX];
    cJSON_AddStringToObject(r, "name", "Température");
    topic(st, sizeof(st), "sensor/temperature");
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "device_class", "temperature");
    cJSON_AddStringToObject(r, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(r, "state_class", "measurement");
}

static void fill_ntc_offset(cJSON *r, int idx)
{
    (void)idx;
    char st[TOPIC_MAX], cmd[TOPIC_MAX];
    cJSON_AddStringToObject(r, "name", "Offset sonde température");
    topic(st, sizeof(st), "number/ntc_offset/state");
    topic(cmd, sizeof(cmd), "number/ntc_offset/set");
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "command_topic", cmd);
    cJSON_AddNumberToObject(r, "min", -20.0);
    cJSON_AddNumberToObject(r, "max", 20.0);
    cJSON_AddNumberToObject(r, "step", 0.1);
    cJSON_AddStringToObject(r, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(r, "mode", "box");
    cJSON_AddStringToObject(r, "entity_category", "config");
}

static void fill_rssi(cJSON *r, int idx)
{
    (void)idx;
    char st[TOPIC_MAX];
    cJSON_AddStringToObject(r, "name", "Signal WiFi");
    topic(st, sizeof(st), "sensor/rssi");
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "device_class", "signal_strength");
    cJSON_AddStringToObject(r, "unit_of_measurement", "dBm");
    cJSON_AddStringToObject(r, "entity_category", "diagnostic");
    cJSON_AddStringToObject(r, "state_class", "measurement");
}

static void fill_uptime(cJSON *r, int idx)
{
    (void)idx;
    char st[TOPIC_MAX];
    cJSON_AddStringToObject(r, "name", "Uptime");
    topic(st, sizeof(st), "sensor/uptime");
    cJSON_AddStringToObject(r, "state_topic", st);
    cJSON_AddStringToObject(r, "unit_of_measurement", "s");
    cJSON_AddStringToObject(r, "entity_category", "diagnostic");
    cJSON_AddStringToObject(r, "state_class", "total_increasing");
}

static void publish_all_discovery(void)
{
    publish_discovery_entity("switch", "relais_1", fill_relay, 0);
    publish_discovery_entity("switch", "relais_2", fill_relay, 1);
    publish_discovery_entity("event", "bouton_1", fill_button_event, 0);
    publish_discovery_entity("event", "bouton_2", fill_button_event, 1);
    publish_discovery_entity("switch", "bouton_1_detache", fill_detach, 0);
    publish_discovery_entity("switch", "bouton_2_detache", fill_detach, 1);
    publish_discovery_entity("sensor", "temperature", fill_temperature, 0);
    publish_discovery_entity("number", "offset_temperature", fill_ntc_offset, 0);
    publish_discovery_entity("sensor", "rssi", fill_rssi, 0);
    publish_discovery_entity("sensor", "uptime", fill_uptime, 0);
}

/* ---- États courants republiés après (re)connexion ---- */

static void publish_current_states(void)
{
    char t[TOPIC_MAX], buf[16];

    topic(t, sizeof(t), "status");
    publish(t, "online", true);

    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++) {
        topic(t, sizeof(t), "relay/%d/state", i + 1);
        publish(t, nspanel_relay_get(i) ? "ON" : "OFF", true);
        topic(t, sizeof(t), "detach/%d/state", i + 1);
        publish(t, nspanel_io_button_detached(i) ? "ON" : "OFF", true);
    }

    float ntc = nspanel_ntc_last_c();
    if (!isnan(ntc)) ha_mqtt_publish_temperature(ntc + settings_ntc_offset_tenths() / 10.0f);

    topic(t, sizeof(t), "number/ntc_offset/state");
    snprintf(buf, sizeof(buf), "%.1f", settings_ntc_offset_tenths() / 10.0f);
    publish(t, buf, true);

    ha_mqtt_publish_diag();
}

/* ---- Commandes reçues (relais, détachement, offset) ---- */

static bool topic_is(const char *topic_str, int len, const char *suffix)
{
    size_t sl = strlen(suffix);
    return (size_t)len >= sl && memcmp(topic_str + len - sl, suffix, sl) == 0;
}

static bool payload_on(const char *data, int len)
{
    return len == 2 && memcmp(data, "ON", 2) == 0;
}

static void handle_command(const char *t, int tlen, const char *data, int dlen)
{
    s_stats.commands_received++;
    char payload[24];
    int n = dlen < (int)sizeof(payload) - 1 ? dlen : (int)sizeof(payload) - 1;
    memcpy(payload, data, n);
    payload[n] = '\0';

    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++) {
        char suf[24];
        snprintf(suf, sizeof(suf), "relay/%d/set", i + 1);
        if (topic_is(t, tlen, suf)) {
            bool on = payload_on(payload, n);
            nspanel_relay_set(i, on);
            ha_mqtt_publish_relay(i, on);
            return;
        }
        snprintf(suf, sizeof(suf), "detach/%d/set", i + 1);
        if (topic_is(t, tlen, suf)) {
            bool on = payload_on(payload, n);
            nspanel_io_set_button_detached(i, on);
            ha_mqtt_publish_detached(i, on);
            return;
        }
    }
    if (topic_is(t, tlen, "number/ntc_offset/set")) {
        int tenths = (int)lroundf(strtof(payload, NULL) * 10.0f);
        settings_save_ntc_offset_tenths(tenths);
        char st[TOPIC_MAX], buf[16];
        topic(st, sizeof(st), "number/ntc_offset/state");
        snprintf(buf, sizeof(buf), "%.1f", settings_ntc_offset_tenths() / 10.0f);
        publish(st, buf, true);
        float ntc = nspanel_ntc_last_c();
        if (!isnan(ntc)) ha_mqtt_publish_temperature(ntc + settings_ntc_offset_tenths() / 10.0f);
    }
}

static void subscribe_commands(void)
{
    char t[TOPIC_MAX];
    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++) {
        topic(t, sizeof(t), "relay/%d/set", i + 1);
        esp_mqtt_client_subscribe(s_client, t, 0);
        topic(t, sizeof(t), "detach/%d/set", i + 1);
        esp_mqtt_client_subscribe(s_client, t, 0);
    }
    topic(t, sizeof(t), "number/ntc_offset/set");
    esp_mqtt_client_subscribe(s_client, t, 0);
    esp_mqtt_client_subscribe(s_client, DISCOVERY_PREFIX "/status", 0);
}

/* ---- Événements esp-mqtt ---- */

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t evt = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_stats.connected = true;
        s_stats.connects++;
        ESP_LOGI(TAG, "connecté au broker");
        subscribe_commands();
        publish_all_discovery();
        publish_current_states();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_stats.connected = false;
        s_stats.disconnects++;
        ESP_LOGW(TAG, "déconnecté du broker");
        break;
    case MQTT_EVENT_DATA:
        if (evt->topic_len == (int)strlen(DISCOVERY_PREFIX "/status") &&
            memcmp(evt->topic, DISCOVERY_PREFIX "/status", evt->topic_len) == 0) {
            if (evt->data_len == 6 && memcmp(evt->data, "online", 6) == 0) {
                ESP_LOGI(TAG, "HA redémarré -> republication découverte + états");
                publish_all_discovery();
                publish_current_states();
            }
            break;
        }
        handle_command(evt->topic, evt->topic_len, evt->data, evt->data_len);
        break;
    default:
        break;
    }
}

/* ---- API publique ---- */

esp_err_t ha_mqtt_start(void)
{
    if (!app_config_mqtt_set()) {
        ESP_LOGI(TAG, "aucun broker configuré, MQTT désactivé");
        return ESP_OK;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node, sizeof(s_node), "nspanel_%02x%02x%02x", mac[3], mac[4], mac[5]);

    char will_topic[TOPIC_MAX];
    topic(will_topic, sizeof(will_topic), "status");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = app_config_mqtt_uri(),
        .credentials.username = app_config_mqtt_user()[0] ? app_config_mqtt_user() : NULL,
        .credentials.authentication.password = app_config_mqtt_pass()[0] ? app_config_mqtt_pass() : NULL,
        .credentials.client_id = s_node,
        .session.last_will.topic = will_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 0,
        .session.last_will.retain = true,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

bool ha_mqtt_is_connected(void) { return s_stats.connected; }

void ha_mqtt_publish_relay(int idx, bool on)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return;
    char t[TOPIC_MAX];
    topic(t, sizeof(t), "relay/%d/state", idx + 1);
    publish(t, on ? "ON" : "OFF", true);
}

void ha_mqtt_publish_button(int idx, bool long_press)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return;
    char t[TOPIC_MAX], body[40];
    topic(t, sizeof(t), "button/%d/event", idx + 1);
    snprintf(body, sizeof(body), "{\"event_type\":\"%s\"}", long_press ? "long_press" : "press");
    publish(t, body, false);
}

void ha_mqtt_publish_detached(int idx, bool detached)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return;
    char t[TOPIC_MAX];
    topic(t, sizeof(t), "detach/%d/state", idx + 1);
    publish(t, detached ? "ON" : "OFF", true);
}

void ha_mqtt_publish_temperature(float celsius)
{
    if (isnan(celsius)) return;
    char t[TOPIC_MAX], buf[16];
    topic(t, sizeof(t), "sensor/temperature");
    snprintf(buf, sizeof(buf), "%.1f", celsius);
    publish(t, buf, true);
}

void ha_mqtt_publish_diag(void)
{
    char t[TOPIC_MAX], buf[16];

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        topic(t, sizeof(t), "sensor/rssi");
        snprintf(buf, sizeof(buf), "%d", ap.rssi);
        publish(t, buf, true);
    }

    topic(t, sizeof(t), "sensor/uptime");
    snprintf(buf, sizeof(buf), "%lld", (long long)(esp_timer_get_time() / 1000000));
    publish(t, buf, true);
}

void ha_mqtt_get_stats(ha_mqtt_stats_t *out)
{
    if (out) *out = s_stats;
}

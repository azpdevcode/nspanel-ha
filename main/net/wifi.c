/*
 * Couche réseau : connexion WiFi station native (ESP32 D0WD).
 *
 * Les identifiants (SSID / mot de passe) proviennent de app_config (NVS), et
 * sont configurables à chaud depuis le panneau via wifi_apply() / wifi_scan().
 */
#include "wifi.h"
#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Reconnexion en arrière-plan (backoff borné) : une fois le budget de retries
 * rapides épuisé, on NE renonce JAMAIS — on replanifie via esp_timer jusqu'à
 * reconnexion. Panneau mural toujours allumé : une coupure réseau prolongée
 * (reboot box, maintenance AP) ne doit pas exiger un reboot manuel. */
#define WIFI_RECONNECT_MIN_MS  2000
#define WIFI_RECONNECT_MAX_MS  30000

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = WIFI_RECONNECT_MIN_MS;

/* Remplit une wifi_config_t STA depuis app_config (ou ssid/pass fournis). */
static void fill_sta_config(wifi_config_t *wc, const char *ssid, const char *pass)
{
    memset(wc, 0, sizeof(*wc));
    strncpy((char *)wc->sta.ssid, ssid, sizeof(wc->sta.ssid) - 1);
    strncpy((char *)wc->sta.password, pass, sizeof(wc->sta.password) - 1);
    /* Réseau ouvert (sans mot de passe) → seuil OPEN, sinon WPA2_PSK minimum. */
    wc->sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

/* Tentative de reconnexion différée (callback esp_timer, contexte tâche timer).
 * Un échec relancera STA_DISCONNECTED, qui replanifiera avec un backoff plus long. */
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Tentative de reconnexion WiFi…");
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        /* Pas de SSID configuré → on attend que l'utilisateur configure le WiFi
         * depuis le panneau (wifi_apply). Évite une boucle de retries à vide. */
        if (app_config_wifi_set()) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!app_config_wifi_set()) return;
        /* On n'est plus connecté : effacer le bit (sinon wifi_is_connected() ment
         * après une coupure → icône WiFi figée en « connecté »). */
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_retry_count < CONFIG_WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "Disconnected, retry %d/%d", s_retry_count, CONFIG_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            /* Budget de retries rapides épuisé : on signale l'échec (UI) mais on NE
             * renonce PAS — replanification en arrière-plan avec backoff borné. */
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);   /* défensif (timer one-shot) */
                esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
                ESP_LOGW(TAG, "WiFi KO après %d retries — nouvelle tentative dans %u ms",
                         CONFIG_WIFI_MAX_RETRY, (unsigned)s_backoff_ms);
                s_backoff_ms *= 2;
                if (s_backoff_ms > WIFI_RECONNECT_MAX_MS) s_backoff_ms = WIFI_RECONNECT_MAX_MS;
            }
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_backoff_ms = WIFI_RECONNECT_MIN_MS;   /* reset du backoff après succès */
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t treconnect = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    s_backoff_ms = WIFI_RECONNECT_MIN_MS;
    esp_err_t terr = esp_timer_create(&treconnect, &s_reconnect_timer);
    if (terr != ESP_OK)   /* non bloquant : sans timer on garde les retries rapides */
        ESP_LOGW(TAG, "esp_timer_create(reconnect): %s", esp_err_to_name(terr));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_ip_event, NULL, NULL));

    wifi_config_t wc;
    fill_sta_config(&wc, app_config_wifi_ssid(), app_config_wifi_pass());

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Désactive le modem-sleep WiFi (défaut IDF = MIN_MODEM). Le panneau est un
     * interrupteur mural : un relais local doit se refléter sur HA sans latence
     * de réveil radio, et le WebSocket doit rester réactif aux pushes d'état.
     * Panneau alimenté en permanence → radio toujours active. */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) ESP_LOGW(TAG, "esp_wifi_set_ps(NONE): %s", esp_err_to_name(ps));

    if (app_config_wifi_set())
        ESP_LOGI(TAG, "Connecting to SSID '%s'...", app_config_wifi_ssid());
    else
        ESP_LOGW(TAG, "Aucun WiFi configuré — en attente de configuration (panneau)");
    return ESP_OK;
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_events) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_is_connected(void)
{
    if (!s_wifi_events) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_failed(void)
{
    if (!s_wifi_events) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_FAIL_BIT) != 0;
}

esp_err_t wifi_scan(wifi_ap_record_t *out, int max, int *found)
{
    if (found) *found = 0;
    if (!out || max <= 0) return ESP_ERR_INVALID_ARG;

    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* bloquant */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return err;
    }
    uint16_t n = (uint16_t)max;
    err = esp_wifi_scan_get_ap_records(&n, out);      /* libère la liste interne */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_ap_records: %s", esp_err_to_name(err));
        return err;
    }
    if (found) *found = n;
    ESP_LOGI(TAG, "scan: %u réseaux", n);
    return ESP_OK;
}

esp_err_t wifi_apply(const char *ssid, const char *pass)
{
    if (!ssid) ssid = "";
    if (!pass) pass = "";

    esp_err_t err = app_config_set_wifi(ssid, pass);   /* persiste d'abord */
    if (err != ESP_OK) return err;

    s_retry_count = 0;
    s_backoff_ms = WIFI_RECONNECT_MIN_MS;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    if (s_wifi_events)
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wc;
    fill_sta_config(&wc, ssid, pass);

    esp_wifi_disconnect();   /* coupe l'éventuelle connexion en cours */
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Reconnexion → SSID '%s'", ssid);
    return esp_wifi_connect();
}

bool wifi_get_ip(char *out, size_t n)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;
    snprintf(out, n, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

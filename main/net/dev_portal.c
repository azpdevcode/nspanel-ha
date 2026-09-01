#include "dev_portal.h"
#include "config_portal.h"
#include "nextion.h"
#include "tft_ota.h"
#include "app_ota.h"
#include "ui.h"
#include "wifi.h"
#include "app_config.h"
#include "nspanel_io.h"
#include "ha_ws.h"
#include "rooms.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "dev_portal";

/* ---- Décodage application/x-www-form-urlencoded ----
 * httpd_query_key_value() extrait la valeur brute SANS décoder le
 * percent-encoding (ex. curl --data-urlencode envoie "http%3A%2F%2F..."). */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && hex_val(in[i + 1]) >= 0 && hex_val(in[i + 2]) >= 0) {
            out[o++] = (char)(hex_val(in[i + 1]) * 16 + hex_val(in[i + 2]));
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static esp_err_t get_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app->version);
    cJSON_AddStringToObject(root, "idf_version", app->idf_ver);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    bool connected = wifi_is_connected();
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddBoolToObject(wifi, "configured", app_config_wifi_set());
    char ip[16] = "";
    if (connected && wifi_get_ip(ip, sizeof(ip))) cJSON_AddStringToObject(wifi, "ip", ip);
    else cJSON_AddNullToObject(wifi, "ip");

    cJSON *ha = cJSON_AddObjectToObject(root, "ha");
    cJSON_AddBoolToObject(ha, "configured", app_config_ha_set());
    cJSON_AddBoolToObject(ha, "ws_connected", ha_ws_is_authed());   /* auth_ok reçu */
    cJSON_AddBoolToObject(ha, "ws_authed", ha_ws_is_ready());       /* authed + abonné */
    cJSON_AddBoolToObject(ha, "auth_invalid", ha_ws_auth_invalid());
    ha_ws_stats_t wst;
    ha_ws_get_stats(&wst);
    cJSON_AddNumberToObject(ha, "ws_events", (double)wst.events_received);
    cJSON_AddNumberToObject(ha, "ws_truncated", (double)wst.frames_truncated);
    cJSON_AddNumberToObject(ha, "ws_reconnects", (double)wst.reconnects);
    cJSON_AddBoolToObject(ha, "rooms_fetching", rooms_fetching());
    cJSON_AddNumberToObject(ha, "room_current", rooms_current());
    cJSON_AddNumberToObject(ha, "lights_total", rooms_lights_total());
    cJSON_AddNumberToObject(ha, "lights_on", rooms_lights_on());
    cJSON *rooms = cJSON_AddArrayToObject(ha, "rooms");
    for (int i = 0; i < rooms_count(); i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", rooms_name(i));
        cJSON_AddNumberToObject(r, "lights", rooms_light_count(i));
        cJSON_AddStringToObject(r, "climate", rooms_climate_entity(i));
        cJSON_AddItemToArray(rooms, r);
    }

    cJSON *screen = cJSON_AddObjectToObject(root, "screen");
    cJSON_AddBoolToObject(screen, "gpio4_powered", nspanel_io_screen_powered());
    cJSON_AddNumberToObject(screen, "baud", (double)nextion_current_baud());
    cJSON_AddBoolToObject(screen, "vspec_present", nextion_spec_present());
    cJSON_AddNumberToObject(screen, "vspec_rev", (double)nextion_spec_revision());
    int present = 0, total = 0;
    nextion_objects_summary(&present, &total);
    cJSON_AddNumberToObject(screen, "objects_present", present);
    cJSON_AddNumberToObject(screen, "objects_total", total);

    cJSON *pages = cJSON_AddObjectToObject(screen, "pages");
#define NX_PAGE(id, name) cJSON_AddBoolToObject(pages, name, nx_page_present(id));
#include "nx_pages.def"
#undef NX_PAGE

    static const char *const ota_state_name[] = {
        [TFT_OTA_IDLE] = "idle", [TFT_OTA_RUNNING] = "running",
        [TFT_OTA_OK] = "ok", [TFT_OTA_FAILED] = "failed",
    };
    cJSON *ota = cJSON_AddObjectToObject(root, "tft_ota");
    cJSON_AddStringToObject(ota, "state", ota_state_name[tft_ota_get_state()]);
    cJSON_AddStringToObject(ota, "status", tft_ota_get_status());
    cJSON_AddStringToObject(ota, "nx_last_step", nextion_last_step());

    static const char *const app_ota_state_name[] = {
        [APP_OTA_IDLE] = "idle", [APP_OTA_RUNNING] = "running",
        [APP_OTA_OK] = "ok", [APP_OTA_FAILED] = "failed",
    };
    cJSON *aota = cJSON_AddObjectToObject(root, "app_ota");
    cJSON_AddStringToObject(aota, "state", app_ota_state_name[app_ota_get_state()]);
    cJSON_AddStringToObject(aota, "status", app_ota_get_status());

    /* Journal tactile : sert à relever les `id` des hotspots de navigation,
     * que rien d'autre n'expose (voir ui.h). */
    ui_touch_rec_t touches[16];
    int nt = ui_touch_history(touches, 16);
    cJSON *tl = cJSON_AddArrayToObject(root, "touches");
    for (int i = 0; i < nt; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "page", touches[i].page);
        cJSON_AddNumberToObject(e, "cid", touches[i].cid);
        cJSON_AddBoolToObject(e, "pressed", touches[i].pressed);
        cJSON_AddNumberToObject(e, "uptime_s", touches[i].uptime_s);
        cJSON_AddItemToArray(tl, e);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

static void bridge_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));   /* laisse la réponse HTTP partir */
    nextion_bridge_run();             /* noreturn */
}

static esp_err_t post_bridge(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Pont série actif — téléversez depuis Nextion Editor, "
                             "puis redémarrez le panneau.");
    xTaskCreate(bridge_task, "nx_bridge", 3072, NULL, 5, NULL);
    return ESP_OK;
}

static void tft_ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err = tft_ota_run(url);
    ESP_LOGI(TAG, "tft_ota_run(%s): %s", url, esp_err_to_name(err));
    free(url);
    vTaskDelete(NULL);
}

static void app_ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err = app_ota_run(url);   /* ne rend la main qu'en cas d'échec */
    ESP_LOGE(TAG, "app_ota_run(%s): %s", url, esp_err_to_name(err));
    free(url);
    vTaskDelete(NULL);
}

/* Corps commun à /tft et /ota : les deux prennent un `url=` en
 * application/x-www-form-urlencoded et démarrent une tâche de fond. Le duplicat
 * n'apportait rien d'autre que deux endroits où oublier le url_decode(). */
static esp_err_t start_url_task(httpd_req_t *req, TaskFunction_t fn,
                                const char *task_name, uint32_t stack,
                                const char *reply)
{
    char raw[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(raw))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corps invalide");
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, raw + received, total - received);
        if (r <= 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "lecture");
        received += r;
    }
    raw[total] = '\0';

    char url_enc[256];
    if (httpd_query_key_value(raw, "url", url_enc, sizeof(url_enc)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url manquante");

    char url_dec[256];
    url_decode(url_enc, url_dec, sizeof(url_dec));

    char *url = strdup(url_dec);
    if (!url) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, reply);
    if (xTaskCreate(fn, task_name, stack, url, 5, NULL) != pdPASS) {
        free(url);
        ESP_LOGE(TAG, "xTaskCreate(%s) a échoué", task_name);
    }
    return ESP_OK;
}

static esp_err_t post_ota(httpd_req_t *req)
{
    if (app_ota_get_state() == APP_OTA_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Une mise à jour applicative est déjà en cours — voir /status.");
        return ESP_OK;
    }
    /* Pas pendant un transfert TFT : les deux se disputeraient la bande passante
     * WiFi et app_ota écrit sur l'écran pour afficher sa progression. */
    if (tft_ota_get_state() == TFT_OTA_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Un transfert TFT est en cours — réessayez après.");
        return ESP_OK;
    }
    /* 8 Ko comme tft_ota : esp_https_ota embarque le client HTTP et ses frames
     * TLS, compilées même pour une URL en http:// simple. */
    return start_url_task(req, app_ota_task, "app_ota", 8192,
                          "Mise à jour firmware démarrée — voir /status.");
}

static esp_err_t post_tft(httpd_req_t *req)
{
    /* Un seul transfert à la fois. Le bus écran a désormais un verrou
     * (nx_bus_acquire), donc deux transferts ne s'entrelaceraient plus sur
     * l'UART — mais le second attendrait 10 s puis échouerait, et rien
     * n'attend ces tâches : autant refuser franchement ici. */
    if (tft_ota_get_state() == TFT_OTA_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Un transfert TFT est déjà en cours — voir /status.");
        return ESP_OK;
    }
    if (app_ota_get_state() == APP_OTA_RUNNING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Une mise à jour firmware est en cours — réessayez après.");
        return ESP_OK;
    }
    /* 8 Ko : le tampon de transfert de tft_ota_run() (4 Ko, cf. CHUNK_SIZE) est
     * une variable locale sur CETTE pile, plus les frames internes
     * d'esp_http_client (TLS compilé même pour du HTTP simple). 4 Ko seul a
     * déjà causé un débordement de pile (reset en pleine requête). */
    return start_url_task(req, tft_ota_task, "tft_ota", 8192,
                          "Transfert TFT démarré — voir /status.");
}

/* POST /nx : REPL série vers l'écran, via HTTP. Corps form-urlencoded :
 *   cmd=<commande Nextion>   (vide = écoute seule)
 *   baud=<débit>             (optionnel : force ce débit pour l'échange)
 *   window=<ms>              (optionnel : fenêtre d'écoute, défaut 400, max 3000)
 * Réponse JSON : la commande envoyée et les octets reçus en hexadécimal. */
static esp_err_t post_nx(httpd_req_t *req)
{
    char raw[320];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(raw))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corps invalide");
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, raw + received, total - received);
        if (r <= 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "lecture");
        received += r;
    }
    raw[total] = '\0';

    char enc[256], cmd[256] = "";
    if (httpd_query_key_value(raw, "cmd", enc, sizeof(enc)) == ESP_OK)
        url_decode(enc, cmd, sizeof(cmd));

    char tmp[16];
    uint32_t baud = 0, window = 400;
    if (httpd_query_key_value(raw, "baud", tmp, sizeof(tmp)) == ESP_OK)
        baud = (uint32_t)atoi(tmp);
    if (httpd_query_key_value(raw, "window", tmp, sizeof(tmp)) == ESP_OK)
        window = (uint32_t)atoi(tmp);
    if (window > 3000) window = 3000;

    uint8_t rx[256];
    size_t n = nx_raw_exchange(cmd, baud, rx, sizeof(rx), window);

    char hex[3 * sizeof(rx) + 1];
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", rx[i]);
    if (o > 0) hex[o - 1] = '\0'; else hex[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddNumberToObject(root, "baud", (double)baud);
    cJSON_AddNumberToObject(root, "window_ms", (double)window);
    cJSON_AddNumberToObject(root, "rx_len", (double)n);
    cJSON_AddStringToObject(root, "rx_hex", hex);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* POST /scan : rebalaye les débits jusqu'à ce que l'écran réponde, puis re-sonde.
 * À utiliser après un upload de .tft qui a changé le débit de l'écran. */
static esp_err_t post_scan(httpd_req_t *req)
{
    uint32_t baud = nextion_scan_baud();
    if (baud) nextion_probe();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "baud", (double)baud);
    cJSON_AddBoolToObject(root, "vspec_present", nextion_spec_present());
    cJSON_AddNumberToObject(root, "vspec_rev", (double)nextion_spec_revision());
    int present = 0, total = 0;
    nextion_objects_summary(&present, &total);
    cJSON_AddNumberToObject(root, "objects_present", present);
    cJSON_AddNumberToObject(root, "objects_total", total);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* POST /rooms : force un nouveau fetch du modèle de pièces sans attendre le
 * reboot ni la relance périodique (~30 min) — utile juste après avoir saisi le
 * compte HA, ou après un changement d'aires côté HA. */
static esp_err_t post_rooms(httpd_req_t *req)
{
    rooms_start_fetch();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Rafraîchissement du modèle de pièces démarré — voir /status.");
    return ESP_OK;
}

esp_err_t dev_portal_register(void)
{
    httpd_handle_t h = config_portal_handle();
    if (!h) {
        ESP_LOGE(TAG, "config_portal non démarré");
        return ESP_ERR_INVALID_STATE;
    }
    httpd_uri_t bridge = { .uri = "/bridge", .method = HTTP_POST, .handler = post_bridge };
    httpd_uri_t tft     = { .uri = "/tft",    .method = HTTP_POST, .handler = post_tft };
    httpd_uri_t status  = { .uri = "/status", .method = HTTP_GET,  .handler = get_status };
    httpd_uri_t nx      = { .uri = "/nx",     .method = HTTP_POST, .handler = post_nx };
    httpd_uri_t scan    = { .uri = "/scan",   .method = HTTP_POST, .handler = post_scan };
    httpd_uri_t ota     = { .uri = "/ota",    .method = HTTP_POST, .handler = post_ota };
    httpd_uri_t rooms   = { .uri = "/rooms",  .method = HTTP_POST, .handler = post_rooms };
    httpd_register_uri_handler(h, &bridge);
    httpd_register_uri_handler(h, &tft);
    httpd_register_uri_handler(h, &status);
    httpd_register_uri_handler(h, &nx);
    httpd_register_uri_handler(h, &scan);
    httpd_register_uri_handler(h, &ota);
    httpd_register_uri_handler(h, &rooms);
    ESP_LOGI(TAG, "routes de développement prêtes (/bridge, /tft, /ota, /status, /nx, /scan, /rooms)");
    return ESP_OK;
}

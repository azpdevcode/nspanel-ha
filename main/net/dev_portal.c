#include "dev_portal.h"
#include "config_portal.h"
#include "nextion.h"
#include "tft_ota.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dev_portal";

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

static esp_err_t post_tft(httpd_req_t *req)
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

    char *url = strdup(url_enc);
    if (!url) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Transfert TFT démarré — voir les logs série.");
    xTaskCreate(tft_ota_task, "tft_ota", 4096, url, 5, NULL);
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
    httpd_register_uri_handler(h, &bridge);
    httpd_register_uri_handler(h, &tft);
    ESP_LOGI(TAG, "routes de développement prêtes (/bridge, /tft)");
    return ESP_OK;
}

#include "tft_ota.h"
#include "nextion.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "tft_ota";

#define CHUNK_SIZE      4096
#define UPLOAD_BAUD     115200   /* débit conservateur pour l'upload, indépendant
                                   * du débit de croisière (921600/115200) */
#define CHUNK_TIMEOUT_MS  4000

esp_err_t tft_ota_run(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t total = esp_http_client_fetch_headers(c);
    if (total <= 0) {
        ESP_LOGE(TAG, "taille de contenu invalide (%lld) — url=%s", (long long)total, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "téléchargement %s (%lld octets) -> écran", url, (long long)total);

    if (!nx_tft_upload_begin((uint32_t)total, UPLOAD_BAUD, 3000)) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    uint8_t buf[CHUNK_SIZE];
    int64_t remaining = total;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        int want = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : (int)remaining;
        int n = esp_http_client_read(c, (char *)buf, want);
        if (n <= 0) {
            ESP_LOGE(TAG, "lecture HTTP interrompue (%d octets restants)", (int)remaining);
            result = ESP_FAIL;
            break;
        }
        if (!nx_tft_upload_chunk(buf, (size_t)n, CHUNK_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "écran : accusé manquant, transfert interrompu");
            result = ESP_FAIL;
            break;
        }
        remaining -= n;
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    nx_tft_upload_end();

    if (result == ESP_OK) ESP_LOGI(TAG, "upload TFT terminé, écran redémarré");
    return result;
}

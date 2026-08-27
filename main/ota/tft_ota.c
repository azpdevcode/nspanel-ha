#include "tft_ota.h"
#include "nextion.h"
#include "app_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "tft_ota";

#define CHUNK_SIZE      4096
#define UPLOAD_BAUD     115200   /* débit conservateur pour l'upload, indépendant
                                   * du débit de croisière (921600/115200) */
#define CHUNK_TIMEOUT_MS  4000

/* État exposé à /status : seule façon de diagnostiquer un échec sur un
 * panneau qu'on ne peut pas monitorer en série (câblage/alim indisponibles
 * en simultané sur ce montage). */
static tft_ota_state_t s_state = TFT_OTA_IDLE;
static char s_status[160] = "jamais lancé";

static void set_status(tft_ota_state_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    s_state = st;
    if (st == TFT_OTA_FAILED) ESP_LOGE(TAG, "%s", s_status);
    else ESP_LOGI(TAG, "%s", s_status);
}

tft_ota_state_t tft_ota_get_state(void) { return s_state; }
const char *tft_ota_get_status(void) { return s_status; }

/* Mise à jour de progression, sans spammer les logs (appelée par bloc de 4 Ko). */
static void set_progress(int64_t done, int64_t total)
{
    snprintf(s_status, sizeof(s_status), "transfert en cours : %lld/%lld octets (%d%%)",
             (long long)done, (long long)total, (int)(total ? (done * 100 / total) : 0));
    s_state = TFT_OTA_RUNNING;
}

esp_err_t tft_ota_run(const char *url)
{
    if (!url || !url[0]) {
        set_status(TFT_OTA_FAILED, "url manquante/vide");
        return ESP_ERR_INVALID_ARG;
    }

    set_status(TFT_OTA_RUNNING, "connexion à %s…", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        set_status(TFT_OTA_FAILED, "esp_http_client_init a échoué (url malformée ? %s)", url);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        set_status(TFT_OTA_FAILED, "connexion HTTP échouée: %s (url=%s)", esp_err_to_name(err), url);
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t total = esp_http_client_fetch_headers(c);
    if (total <= 0) {
        set_status(TFT_OTA_FAILED, "taille de contenu invalide (%lld) — url=%s",
                   (long long)total, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    set_status(TFT_OTA_RUNNING, "téléchargement (%lld octets) -> écran", (long long)total);

    if (!nx_tft_upload_begin((uint32_t)total, UPLOAD_BAUD, 3000)) {
        set_status(TFT_OTA_FAILED, "écran : pas d'accusé initial (whmi-wri) — écran muet ou occupé");
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        set_status(TFT_OTA_FAILED, "malloc(%d) du tampon de transfert a échoué", CHUNK_SIZE);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }

    int64_t remaining = total;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        int want = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : (int)remaining;
        int n = esp_http_client_read(c, (char *)buf, want);
        if (n <= 0) {
            set_status(TFT_OTA_FAILED, "lecture HTTP interrompue (%d octets restants sur %lld)",
                       (int)remaining, (long long)total);
            result = ESP_FAIL;
            break;
        }
        if (!nx_tft_upload_chunk(buf, (size_t)n, CHUNK_TIMEOUT_MS)) {
            set_status(TFT_OTA_FAILED, "écran : accusé manquant, transfert interrompu (%d/%lld octets)",
                       (int)(total - remaining), (long long)total);
            result = ESP_FAIL;
            break;
        }
        remaining -= n;
        set_progress(total - remaining, total);
    }
    free(buf);

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    nx_tft_upload_end();

    if (result == ESP_OK)
        set_status(TFT_OTA_OK, "upload terminé (%lld octets), écran redémarré", (long long)total);
    return result;
}

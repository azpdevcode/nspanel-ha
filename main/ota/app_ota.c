#include "app_ota.h"
#include "nextion.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_ota";

/* Même raison d'être que dans tft_ota.c : sur ce montage on ne peut pas écouter
 * la liaison série, donc tout diagnostic passe par /status. */
static app_ota_state_t s_state = APP_OTA_IDLE;
static char s_status[160] = "jamais lancé";

static void set_status(app_ota_state_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    s_state = st;
    if (st == APP_OTA_FAILED) ESP_LOGE(TAG, "%s", s_status);
    else ESP_LOGI(TAG, "%s", s_status);
}

app_ota_state_t app_ota_get_state(void) { return s_state; }
const char *app_ota_get_status(void) { return s_status; }

/* L'écran est le seul retour visuel pour qui est devant le panneau. jProg et
 * tStat sont sur la page boot ; l'adressage qualifié les atteint même si une
 * autre page est affichée, et boot_status_tick() se tait pendant ce temps pour
 * ne pas réécrire tStat par-dessus (voir app_main.c). */
static void show_progress(int pct, int done, int total)
{
    static int last_pct = -1;
    if (pct == last_pct) return;
    last_pct = pct;

    nx_set_val(BOOT_PROGRESS, pct);
    char line[64];
    snprintf(line, sizeof(line), "Mise à jour firmware\r%d%%  (%d/%d Ko)",
             pct, done / 1024, total / 1024);
    nx_set_txt(BOOT_STATUS, line);

    snprintf(s_status, sizeof(s_status), "écriture en cours : %d/%d octets (%d%%)",
             done, total, pct);
    s_state = APP_OTA_RUNNING;
}

esp_err_t app_ota_run(const char *url)
{
    if (!url || !url[0]) {
        set_status(APP_OTA_FAILED, "url manquante/vide");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        set_status(APP_OTA_FAILED, "aucune partition OTA libre (table de partitions ?)");
        return ESP_FAIL;
    }
    set_status(APP_OTA_RUNNING, "connexion à %s… (%s -> %s)",
               url, running ? running->label : "?", target->label);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        /* ESP_ERR_INVALID_ARG sur une url http:// = CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
         * manquant : esp_https_ota_begin() refuse avant tout transfert. Le dire
         * ici, sinon le code ne raconte rien et il faut aller lire l'IDF. */
        if (err == ESP_ERR_INVALID_ARG && strncmp(url, "http://", 7) == 0)
            set_status(APP_OTA_FAILED, "url http:// refusée — "
                       "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP absent de ce firmware");
        else
            set_status(APP_OTA_FAILED, "ouverture OTA échouée: %s (url=%s)",
                       esp_err_to_name(err), url);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    /* En-têtes lus : refuser tout de suite une image qui ne tiendrait pas,
     * plutôt que de découvrir le débordement après 90 % du transfert. */
    int total = esp_https_ota_get_image_size(h);
    if (total <= 0 || (size_t)total > target->size) {
        set_status(APP_OTA_FAILED, "taille d'image invalide (%d octets, partition %s = %u)",
                   total, target->label, (unsigned)target->size);
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "image de %d octets -> %s (%u octets)",
             total, target->label, (unsigned)target->size);

    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        show_progress(total ? (int)((int64_t)done * 100 / total) : 0, done, total);
    }

    if (err != ESP_OK) {
        set_status(APP_OTA_FAILED, "transfert interrompu: %s (%d/%d octets)",
                   esp_err_to_name(err), esp_https_ota_get_image_len_read(h), total);
        esp_https_ota_abort(h);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        set_status(APP_OTA_FAILED, "image tronquée (%d/%d octets annoncés)",
                   esp_https_ota_get_image_len_read(h), total);
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }

    /* C'est esp_https_ota_finish() qui valide l'image et bascule la partition
     * de boot. Tant qu'il n'a pas rendu ESP_OK, rien n'a changé. */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
            set_status(APP_OTA_FAILED, "image refusée à la validation "
                                       "(pas un .bin ESP32, ou corrompue)");
        else
            set_status(APP_OTA_FAILED, "finalisation échouée: %s", esp_err_to_name(err));
        return err;
    }

    set_status(APP_OTA_OK, "installé sur %s (%d octets), redémarrage…",
               target->label, total);
    nx_set_val(BOOT_PROGRESS, 100);
    nx_set_txt(BOOT_STATUS, "Mise à jour installée\rredémarrage…");

    /* Laisser la réponse HTTP partir et l'écran afficher le message. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;   /* jamais atteint */
}

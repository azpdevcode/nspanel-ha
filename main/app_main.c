/*
 * Boot séquentiel — phase 1 (squelette + lien série).
 *
 * TRY_INIT : les sous-systèmes secondaires logguent et continuent en cas
 * d'échec ; seuls NVS et l'IO locale (alimentation écran) sont fatals.
 * Voir design/HMI-SPEC.md et le plan de fork pour les phases suivantes
 * (modèle pièces, lumières, chauffage, météo, paramètres).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "wifi.h"
#include "config_portal.h"
#include "dev_portal.h"
#include "nspanel_io.h"
#include "nextion.h"
#include "app_diag.h"

static const char *TAG = "main";

#define TRY_INIT(call, what) do { \
        esp_err_t _e = (call); \
        if (_e != ESP_OK) ESP_LOGW(TAG, "%s: %s (non fatal, on continue)", what, esp_err_to_name(_e)); \
    } while (0)

/* Relais physiques : miroir HA à brancher en phase 4 (settings.c fournira les
 * entités rly1_ent/rly2_ent). Pour l'instant, on trace juste l'événement. */
static void on_relay_local_change(int idx, bool on)
{
    ESP_LOGI(TAG, "relais %d -> %s (miroir HA à câbler en phase 4)", idx + 1, on ? "ON" : "OFF");
}

static void on_longpress(void)
{
    ESP_LOGI(TAG, "re-sondage manuel de l'écran demandé");
    nextion_force_reprobe();
    nextion_probe();
}

static void on_ntc(float c)
{
    if (c == c) ESP_LOGD(TAG, "NTC: %.1f °C", c);   /* c==c écarte NAN sans <math.h> ici */
}

/* Rafraîchit la page boot avec l'état WiFi/HA courant. Appelé en boucle tant
 * que le panneau n'est pas allé plus loin que la phase 1 (pas encore de home). */
static void boot_status_tick(void)
{
    char line[96];
    if (wifi_is_connected()) {
        char ip[16];
        wifi_get_ip(ip, sizeof(ip));
        snprintf(line, sizeof(line), "WiFi OK  %s\nHA: %s",
                 ip, app_config_ha_set() ? app_config_ha_url() : "non configuré");
    } else if (wifi_failed()) {
        snprintf(line, sizeof(line), "WiFi KO — configurez via\nle portail (voir écran suivant)");
    } else {
        snprintf(line, sizeof(line), "Connexion WiFi…");
    }
    nx_set_txt(BOOT_STATUS, line);
}

void app_main(void)
{
    app_diag_log_mem("boot");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    esp_ota_mark_app_valid_cancel_rollback();

    ESP_ERROR_CHECK(nspanel_io_init());
    nspanel_io_set_relay_cb(on_relay_local_change);
    nspanel_io_set_longpress_cb(on_longpress);
    nspanel_io_set_ntc_cb(on_ntc);

    if (nspanel_io_boot_bridge_requested()) {
        /* UART écran nécessaire même en pont : nextion_init() l'installe.
         * Le sondage échouera silencieusement si l'écran attend l'Editor. */
        nextion_init();
        nextion_bridge_run();   /* noreturn */
    }

    ESP_ERROR_CHECK(nextion_init());
    nx_page(NXP_BOOT);
    nx_set_txt(BOOT_STATUS, "Démarrage…");

    app_config_init();

    TRY_INIT(wifi_start(), "wifi_start");

    /* SNTP + fuseau France métropolitaine (CET/CEST). */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    TRY_INIT(config_portal_start(), "config_portal_start");
    TRY_INIT(dev_portal_register(), "dev_portal_register");

    app_diag_start();
    app_diag_log_mem("init done");

    for (;;) {
        boot_status_tick();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

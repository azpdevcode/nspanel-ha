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
#include "ui.h"
#include "app_diag.h"
#include "settings.h"
#include "ha_worker.h"
#include "ha_ws.h"
#include "rooms.h"

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
    /* En dernier : voir config_portal_register_fallback() pour pourquoi l'ordre compte. */
    TRY_INIT(config_portal_register_fallback(), "config_portal_register_fallback");

    /* Après les portails : ui_task écrit l'écran, et /status doit pouvoir
     * répondre même si l'écran est absent. */
    TRY_INIT(ui_start(), "ui_start");

    /* Chaîne HA : chacune de ces étapes doit rester non fatale — sans compte HA
     * configuré, le panneau reste une horloge flashable (cf. plan de fork). */
    settings_load();
    TRY_INIT(ha_worker_start(), "ha_worker_start");
    TRY_INIT(ha_ws_start(), "ha_ws_start");
    rooms_start_fetch();
    TRY_INIT(rooms_watchdog_start(), "rooms_watchdog_start");

    app_diag_start();
    app_diag_log_mem("init done");

    /* Battement de cœur console. Ce montage n'a pas de reset automatique : on ne
     * peut pas provoquer un boot pour vérifier que le firmware tourne, il faut
     * pouvoir le constater à tout moment sur une écoute de quelques secondes. */
    int tick = 0;
    for (;;) {
        if (++tick % 3 == 0) {   /* ~6 s */
            char ip[16] = "-";
            wifi_get_ip(ip, sizeof(ip));
            const char *ha_state = !app_config_ha_set() ? "non configure"
                                  : ha_ws_is_ready()     ? "connecte"
                                  : ha_ws_auth_invalid()  ? "token refuse"
                                  : ha_ws_is_authed()     ? "abonnement..."
                                  :                         "connexion...";
            ESP_LOGI(TAG, "vivant | wifi=%s ip=%s | ha=%s (%d/%d pieces, %d/%d lumieres) | ecran=%s",
                     wifi_is_connected() ? "ok" : (wifi_failed() ? "ko" : "..."),
                     ip, ha_state,
                     rooms_count(), ROOMS_MAX_ROOMS, rooms_lights_on(), rooms_lights_total(),
                     nx_page_present(NXP_BOOT) ? "tft ok" : "tft absent");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

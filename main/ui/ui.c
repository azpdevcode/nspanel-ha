#include "ui.h"
#include "nextion.h"
#include "app_ota.h"
#include "tft_ota.h"
#include "wifi.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define UI_TICK_MS 250

/* Noms français en dur : l'ESP-IDF n'embarque pas les locales, strftime("%a")
 * y rend toujours de l'anglais. */
static const char *const s_days[7] = {
    "dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."
};
static const char *const s_months[12] = {
    "janv.", "févr.", "mars", "avril", "mai", "juin",
    "juil.", "août", "sept.", "oct.", "nov.", "déc."
};

/* ---- Navigation ---- */

typedef struct { nx_page_t from; uint8_t cid; nx_page_t to; } ui_nav_t;

static const ui_nav_t s_nav[] = {
#define NX_NAV(from, cid, to) { from, cid, to },
#include "nx_nav.def"
#undef NX_NAV
};

/* Page réellement affichée, telle que NOUS l'avons demandée. Pas de sendme
 * périodique : l'écran ne change de page que sur notre ordre. */
static nx_page_t s_page = NXP_BOOT;

/* Dernier appui, pour le retour automatique. */
static uint32_t s_last_touch_s;

/* Aucune page secondaire n'a de bouton retour dessiné pour l'instant, et rien
 * ne garantit qu'on pensera à en mettre un partout : sans ce garde-fou, un
 * appui malheureux laisse le panneau bloqué sur une page jusqu'au prochain
 * redémarrage. */
#define UI_HOME_TIMEOUT_S 30

static bool goto_page(nx_page_t p)
{
    if (!nx_page(p)) return false;   /* page absente du .tft : no-op silencieux */
    s_page = p;
    ESP_LOGI(TAG, "page -> %d", (int)p);
    return true;
}

/* ---- Journal tactile ---- */

#define TOUCH_LOG_N 16   /* 4 zones = 8 trames appui+relâche ; 16 laisse
                                * la marge pour une passe complète sans perdre le début */
static ui_touch_rec_t s_touch_log[TOUCH_LOG_N];
static int s_touch_count;   /* total reçu, sert d'index d'écriture modulo N */

int ui_touch_history(ui_touch_rec_t *out, int max)
{
    int have = s_touch_count < TOUCH_LOG_N ? s_touch_count : TOUCH_LOG_N;
    if (max < have) have = max;
    for (int i = 0; i < have; i++)
        out[i] = s_touch_log[(s_touch_count - 1 - i) % TOUCH_LOG_N];
    return have;
}

static void drain_touches(void)
{
    nx_touch_t t;
    while (nextion_get_touch(&t, 0)) {
        s_touch_log[s_touch_count % TOUCH_LOG_N] = (ui_touch_rec_t){
            .page = t.page, .cid = t.cid, .pressed = t.pressed,
            .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        };
        s_touch_count++;
        ESP_LOGI(TAG, "tactile : page=%u cid=%u %s",
                 t.page, t.cid, t.pressed ? "appui" : "relâche");

        /* Sur l'APPUI, pas la relâche : un hotspot dont seul « Send Component
         * ID » du Touch Press Event est coché n'émet jamais de relâche, et rien
         * dans le .tft ne permet de le savoir à l'avance. */
        if (!t.pressed) continue;
        s_last_touch_s = (uint32_t)(esp_timer_get_time() / 1000000);

        for (size_t i = 0; i < sizeof(s_nav) / sizeof(s_nav[0]); i++) {
            if (s_nav[i].from == s_page && s_nav[i].cid == t.cid) {
                goto_page(s_nav[i].to);
                break;
            }
        }
    }
}

/* ---- Pages ---- */

/* L'heure vient de SNTP ; avant la première synchro, time() rend 1970. Tout
 * l'écran home tourne autour de l'horloge, donc on reste sur boot tant qu'elle
 * n'est pas crédible plutôt que d'afficher « 01:00 » pendant dix secondes. */
static bool clock_is_synced(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    return out->tm_year + 1900 >= 2024;
}

static void boot_page_tick(void)
{
    char line[96];
    if (wifi_is_connected()) {
        char ip[16];
        wifi_get_ip(ip, sizeof(ip));
        snprintf(line, sizeof(line), "WiFi OK  %s\rHA: %s",
                 ip, app_config_ha_set() ? app_config_ha_url() : "non configuré");
    } else if (wifi_failed()) {
        snprintf(line, sizeof(line), "WiFi KO — configurez via\rle portail (voir écran suivant)");
    } else {
        snprintf(line, sizeof(line), "Connexion WiFi…");
    }
    nx_set_txt(BOOT_STATUS, line);
}

/* N'écrit que si la valeur a changé : à 4 Hz, réécrire l'heure chaque tick
 * saturerait l'UART pour rien et ferait clignoter le texte. */
#define UI_CACHE_LEN 32

static void set_if_changed(nx_obj_t o, char cache[UI_CACHE_LEN], const char *val)
{
    if (strncmp(cache, val, UI_CACHE_LEN) == 0) return;
    snprintf(cache, UI_CACHE_LEN, "%s", val);
    nx_set_txt(o, val);
}

static void home_page_tick(const struct tm *tm)
{
    static char clock_cache[UI_CACHE_LEN], date_cache[UI_CACHE_LEN];
    char buf[UI_CACHE_LEN];

    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    set_if_changed(HOME_CLOCK, clock_cache, buf);

    snprintf(buf, sizeof(buf), "%s %d %s",
             s_days[tm->tm_wday % 7], tm->tm_mday, s_months[tm->tm_mon % 12]);
    set_if_changed(HOME_DATE, date_cache, buf);
}

static void ui_task(void *arg)
{
    (void)arg;

    goto_page(NXP_BOOT);
    nx_set_txt(BOOT_STATUS, "Démarrage…");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
        drain_touches();

        /* Pendant un transfert, l'écran ne nous appartient pas : app_ota y
         * affiche sa progression, et tft_ota le réécrit entièrement. */
        if (app_ota_get_state() == APP_OTA_RUNNING ||
            tft_ota_get_state() == TFT_OTA_RUNNING) {
            s_page = NXP_BOOT;   /* l'écran redémarre : forcer une renavigation */
            continue;
        }

        struct tm tm;
        bool synced = clock_is_synced(&tm);

        if (s_page == NXP_BOOT) {
            if (synced && nx_page_present(NXP_HOME)) goto_page(NXP_HOME);
            else boot_page_tick();
        } else if (s_page != NXP_HOME) {
            uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            if (now_s - s_last_touch_s >= UI_HOME_TIMEOUT_S) {
                ESP_LOGI(TAG, "inactivité : retour à home");
                goto_page(NXP_HOME);
            }
        }

        if (s_page == NXP_HOME) home_page_tick(&tm);
    }
}

esp_err_t ui_start(void)
{
    /* 4 Ko : uniquement des tampons de quelques dizaines d'octets et snprintf. */
    return xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

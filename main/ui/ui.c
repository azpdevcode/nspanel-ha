#include "ui.h"
#include "nextion.h"
#include "nx_draw.h"
#include "app_ota.h"
#include "tft_ota.h"
#include "wifi.h"
#include "app_config.h"
#include "ha_ws.h"
#include "rooms.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
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

/* ---- Actions (hors navigation) ---- */

typedef enum {
#define NX_ACT(page, cid, act) act,
#include "nx_actions.def"
#undef NX_ACT
} ui_action_t;

typedef struct { nx_page_t page; uint8_t cid; ui_action_t act; } ui_act_t;

static const ui_act_t s_act[] = {
#define NX_ACT(page, cid, act) { page, cid, act },
#include "nx_actions.def"
#undef NX_ACT
};

/* Page réellement affichée, telle que NOUS l'avons demandée. Pas de sendme
 * périodique : l'écran ne change de page que sur notre ordre. */
static nx_page_t s_page = NXP_BOOT;

/* Dernier appui, pour le retour automatique. */
static uint32_t s_last_touch_s;

/* Aucune page secondaire n'a de bouton retour dessiné pour l'instant, et rien
 * ne garantit qu'on pensera à en mettre un partout : sans ce garde-fou, un
 * appui malheureux laisse le panneau bloqué sur une page jusqu'au prochain
 * redémarrage. `cfg` a besoin de plus de temps : on y lit des diagnostics et
 * on glisse des curseurs, pas un aller-retour de quelques secondes. */
#define UI_HOME_TIMEOUT_S 30
#define UI_CFG_TIMEOUT_S 120

static uint32_t ui_timeout_s(void)
{
    return s_page == NXP_CFG ? UI_CFG_TIMEOUT_S : UI_HOME_TIMEOUT_S;
}

/* Un dessin (nx_draw_arc) s'applique à la page affichée et ne survit ni à un
 * changement de page ni à un cycle veille/réveil : ce drapeau fait repeindre
 * les anneaux de home au prochain tick qui la trouve affichée, sans jamais
 * dessiner « en aveugle » sur une autre page. */
static volatile bool s_home_needs_repaint;

/* Écrire `hDim`/`hSleep` à chaque tick ferait sauter le curseur sous le doigt
 * de l'utilisateur : on ne les (re)synchronise qu'à l'entrée sur la page. */
static volatile bool s_cfg_needs_sync;

static bool goto_page(nx_page_t p)
{
    if (!nx_page(p)) return false;   /* page absente du .tft : no-op silencieux */
    s_page = p;
    if (p == NXP_HOME) s_home_needs_repaint = true;
    if (p == NXP_CFG) s_cfg_needs_sync = true;
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

/* ACT_DIM/ACT_SLEEP sont les deux seules actions déclenchées sur RELÂCHE
 * (Slider : la valeur n'est stable qu'une fois le doigt levé) ; toutes les
 * autres actions, comme la navigation, se déclenchent sur APPUI. */
static bool act_is_release(ui_action_t act)
{
    return act == ACT_DIM || act == ACT_SLEEP;
}

static void ui_do_action(ui_action_t act);

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

        if (!t.pressed) {
            /* Slider relâché : c'est le seul cas où la relâche nous intéresse. */
            for (size_t i = 0; i < sizeof(s_act) / sizeof(s_act[0]); i++) {
                if (s_act[i].page == s_page && s_act[i].cid == t.cid &&
                    act_is_release(s_act[i].act)) {
                    s_last_touch_s = (uint32_t)(esp_timer_get_time() / 1000000);
                    ui_do_action(s_act[i].act);
                    break;
                }
            }
            continue;
        }
        s_last_touch_s = (uint32_t)(esp_timer_get_time() / 1000000);

        /* Réveil (0x87) : pas de navigation à faire, mais un dessin en cours
         * pendant la veille n'a pas survécu — le forcer si home est affichée. */
        if (t.page == NX_WAKE_PAGE && t.cid == NX_WAKE_CID) {
            if (s_page == NXP_HOME) s_home_needs_repaint = true;
            continue;
        }

        bool handled = false;
        for (size_t i = 0; i < sizeof(s_nav) / sizeof(s_nav[0]); i++) {
            if (s_nav[i].from == s_page && s_nav[i].cid == t.cid) {
                goto_page(s_nav[i].to);
                handled = true;
                break;
            }
        }
        if (handled) continue;

        for (size_t i = 0; i < sizeof(s_act) / sizeof(s_act[0]); i++) {
            if (s_act[i].page == s_page && s_act[i].cid == t.cid &&
                !act_is_release(s_act[i].act)) {
                ui_do_action(s_act[i].act);
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
        const char *ha;
        if (!app_config_ha_set()) ha = "non configuré (voir portail)";
        else if (ha_ws_is_ready()) ha = "connecté";
        else if (ha_ws_auth_invalid()) ha = "token refusé";
        else if (ha_ws_is_authed()) ha = "abonnement…";
        else ha = "connexion…";
        snprintf(line, sizeof(line), "WiFi OK  %s\rHA: %s", ip, ha);
    } else if (wifi_failed()) {
        snprintf(line, sizeof(line), "WiFi KO — configurez via\rle portail (voir écran suivant)");
    } else {
        snprintf(line, sizeof(line), "Connexion WiFi…");
    }
    nx_set_txt(BOOT_STATUS, line);
}

/* N'écrit que si la valeur a changé : à 4 Hz, réécrire l'heure chaque tick
 * saturerait l'UART pour rien et ferait clignoter le texte.
 *
 * `force` contourne le cache : nx_set_txt() renvoie true dès que l'objet est
 * *présent* dans le .tft (nextion.c), pas quand la commande a réellement
 * atteint l'écran — nx_cmd()/nx_bus_acquire() abandonnent silencieusement en
 * cas de contention (ex. pendant nextion_probe() au boot). Sans resynchro
 * périodique, une écriture perdue dans cette fenêtre reste perdue pour
 * toujours : le cache la croit « envoyée » et plus rien ne la retente, même
 * quand la vraie valeur change ensuite. */
#define UI_CACHE_LEN 32

static void set_if_changed(nx_obj_t o, char cache[UI_CACHE_LEN], const char *val, bool force)
{
    if (!force && strncmp(cache, val, UI_CACHE_LEN) == 0) return;
    snprintf(cache, UI_CACHE_LEN, "%s", val);
    nx_set_txt(o, val);
}

/* Resynchronisation périodique (indépendante du cache) des champs qui ne
 * s'auto-réparent pas naturellement (contrairement à l'horloge, qui change
 * chaque minute) — ~20 s à UI_TICK_MS = 250 ms, loin du seuil de flicker/UART
 * documenté pour HOME_CLOCK/HOME_DATE. */
#define UI_RESYNC_TICKS (20000 / UI_TICK_MS)

static bool resync_due(void)
{
    static uint32_t ticks;
    return (++ticks % UI_RESYNC_TICKS) == 0;
}

/* Écrit un placeholder neutre une seule fois dans les champs pas encore
 * alimentés par le firmware (météo : phase 3, temp. intérieure/NTC : phase 4)
 * — mieux que le "newtxt" par défaut de l'Editor laissé à l'écran. No-op
 * silencieux si l'objet est absent du .tft, comme partout ailleurs. */
static void home_page_placeholders_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    nx_set_txt(HOME_WX_TEMP, "--");
    nx_set_txt(HOME_WX_COND, "--");
}

/* ---- Anneaux de chauffage (page home) ----
 *
 * Nextion n'a pas de primitive d'arc : nx_draw_arc() les rend en chapelet de
 * disques (nx_draw.c). Plage et disposition calquées sur la jauge du panneau
 * P4 (ha-panel/main/ui/page_accueil.c:565-578) : 0..50 °C sur 270°, anneau
 * extérieur = mesure, anneau intérieur = consigne. Géométrie et couleurs
 * doivent rester en phase avec design/EDITOR-HOME.md (tuile Chauffage). */
/* Décalé à gauche de la tuile et abaissé sous le titre « Chauffage » (icône +
 * libellé, y≈52..80 dans gen_images.py:bg_home) : un centrage sur la tuile
 * mettait le trou de l'anneau juste sous le titre, et le texte tTemp/tSet posé
 * dedans (voir plus bas) débordait du trou (coin à ~38 px, bord intérieur sûr
 * à R_IN-TH_IN/2 = 30.5 px) — chaque resynchro périodique de home_page_tick()
 * repeignait alors son rectangle et effaçait l'anneau dessous (pas de
 * framebuffer partagé sur le Nextion, contrairement au P4/LVGL). Anneau plus
 * petit + texte posé à DROITE de l'anneau (zone libre, sans contrainte de
 * rayon) plutôt que dedans : voir design/EDITOR-HOME.md. */
#define HOME_ARC_CX      290
#define HOME_ARC_CY      138
#define HOME_ARC_R_OUT    40
#define HOME_ARC_TH_OUT    8
#define HOME_ARC_R_IN     25
#define HOME_ARC_TH_IN     6
#define HOME_ARC_A_START 135
#define HOME_ARC_A_SWEEP 270
#define HOME_ARC_TMAX      50.0f

/* Couleurs RGB565, arrondies depuis ui_common.h du panneau P4 (voir
 * design/HMI-SPEC.md « Palette ») — pas de source unique côté firmware,
 * resynchroniser à la main si la palette bouge : COL_HEAT_SOFT #f6a17b,
 * COL_HEAT #ee6939, COL_CARD2 #182031. */
#define COL_HEAT_SOFT_565 62735
#define COL_HEAT_565      60231
#define COL_CARD2_565       6406

static int home_arc_end_angle(float temp)
{
    if (temp < 0) temp = 0;
    if (temp > HOME_ARC_TMAX) temp = HOME_ARC_TMAX;
    return HOME_ARC_A_START + (int)lroundf(temp / HOME_ARC_TMAX * HOME_ARC_A_SWEEP);
}

/* Repeint les deux anneaux en entier (piste + indicateur) : appelé seulement
 * quand une valeur a changé ou que la page vient d'apparaître (repaint entrée
 * de page / réveil, voir s_home_needs_repaint), jamais à chaque tick — c'est
 * une rafale de ~120 commandes UART, pas un rafraîchissement de texte. */
static void home_arcs_paint(bool ok, float cur, float set)
{
    /* Débit non négocié mais découvert (nextion_scan_baud()) : à un débit
     * bas, la rafale bloquerait la tâche ui plusieurs secondes (voir
     * design/EDITOR-HOME.md) — dans ce cas, laisser les seuls textes. */
    if (nextion_current_baud() < 57600) return;

    nx_draw_arc(HOME_ARC_CX, HOME_ARC_CY, HOME_ARC_R_OUT, HOME_ARC_TH_OUT,
               HOME_ARC_A_START, HOME_ARC_A_START + HOME_ARC_A_SWEEP, COL_CARD2_565);
    nx_draw_arc(HOME_ARC_CX, HOME_ARC_CY, HOME_ARC_R_IN, HOME_ARC_TH_IN,
               HOME_ARC_A_START, HOME_ARC_A_START + HOME_ARC_A_SWEEP, COL_CARD2_565);
    if (!ok) return;

    int end_cur = home_arc_end_angle(cur), end_set = home_arc_end_angle(set);
    if (end_cur > HOME_ARC_A_START)
        nx_draw_arc(HOME_ARC_CX, HOME_ARC_CY, HOME_ARC_R_OUT, HOME_ARC_TH_OUT,
                   HOME_ARC_A_START, end_cur, COL_HEAT_SOFT_565);
    if (end_set > HOME_ARC_A_START)
        nx_draw_arc(HOME_ARC_CX, HOME_ARC_CY, HOME_ARC_R_IN, HOME_ARC_TH_IN,
                   HOME_ARC_A_START, end_set, COL_HEAT_565);
}

static void home_page_tick(const struct tm *tm)
{
    static char clock_cache[UI_CACHE_LEN], date_cache[UI_CACHE_LEN];
    static char room_cache[UI_CACHE_LEN], lights_cache[UI_CACHE_LEN];
    static char temp_cache[UI_CACHE_LEN], set_cache[UI_CACHE_LEN];
    static int  light_pic_cache = -1;
    static float prev_cur = -1.0f, prev_set = -1.0f;
    static bool  prev_ok;
    char buf[UI_CACHE_LEN];
    bool force = resync_due();

    home_page_placeholders_once();

    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    set_if_changed(HOME_CLOCK, clock_cache, buf, false);

    snprintf(buf, sizeof(buf), "%s %d %s",
             s_days[tm->tm_wday % 7], tm->tm_mday, s_months[tm->tm_mon % 12]);
    set_if_changed(HOME_DATE, date_cache, buf, false);

    if (rooms_model_dirty()) ESP_LOGI(TAG, "modèle pièces à jour, rafraîchissement home");

    /* "pièce ?" (ASCII) plutôt qu'un tiret cadratin : hors Latin-1, il serait
     * de toute façon rendu '?' par latin1_escape_into (nextion.c). */
    int room = rooms_current();
    snprintf(buf, sizeof(buf), "%s", rooms_count() ? rooms_name(room) : "pièce ?");
    set_if_changed(HOME_ROOM, room_cache, buf, force);

    /* Libellé court (cf. design/HMI-SPEC.md: "3 allumées") : la zone tRoom du
     * .tft tronque au-delà d'une quinzaine de caractères. */
    int on = rooms_lights_on(), total = rooms_lights_total();
    if (total == 0) snprintf(buf, sizeof(buf), "aucune lumière");
    else if (on == 0) snprintf(buf, sizeof(buf), "toutes éteintes");
    else snprintf(buf, sizeof(buf), "%d allumée%s", on, on > 1 ? "s" : "");
    set_if_changed(HOME_LIGHTS, lights_cache, buf, force);

    /* Ampoule de la tuile Lumières (65 = éteinte, 66 = allumée, cf.
     * design/images/gen_images.py) : deux images seulement, pas de cache texte. */
    int light_pic = on ? 66 : 65;
    if (light_pic != light_pic_cache || force) {
        nx_set_val(HOME_LIGHT_PIC, light_pic);
        light_pic_cache = light_pic;
    }

    /* Chauffage : entité climate de la pièce affichée (rooms.c). */
    float cur, set_t;
    bool ok;
    rooms_climate_state(room, &cur, &set_t, &ok);

    if (ok) snprintf(buf, sizeof(buf), "%.1f°", cur);
    else    snprintf(buf, sizeof(buf), "--");
    set_if_changed(HOME_TEMP, temp_cache, buf, force);

    if (ok) snprintf(buf, sizeof(buf), "%.0f°", set_t);
    else    snprintf(buf, sizeof(buf), "--");
    set_if_changed(HOME_SET, set_cache, buf, force);

    /* Anneaux : repeints seulement sur changement de valeur ou entrée/réveil
     * de page (s_home_needs_repaint) — jamais à chaque tick, cf. home_arcs_paint(). */
    bool changed = ok != prev_ok ||
                   (ok && (fabsf(cur - prev_cur) > 0.05f || fabsf(set_t - prev_set) > 0.05f));
    if (changed || s_home_needs_repaint) {
        home_arcs_paint(ok, cur, set_t);
        prev_cur = cur; prev_set = set_t; prev_ok = ok;
        s_home_needs_repaint = false;
    }
}

/* ---- Page cfg (Paramètres) ---- */

/* Message temporaire affiché dans tVer à la place de la version, ex. l'invite
 * du portail web pour OTA/TFT (pas de clavier tactile pour une URL). Revient
 * de lui-même : pas de drapeau à réarmer ailleurs. */
static char s_cfg_msg[UI_CACHE_LEN];
static uint32_t s_cfg_msg_until_s;

#define UI_CFG_MSG_S 5

static void cfg_show_msg(const char *msg)
{
    snprintf(s_cfg_msg, sizeof(s_cfg_msg), "%s", msg);
    s_cfg_msg_until_s = (uint32_t)(esp_timer_get_time() / 1000000) + UI_CFG_MSG_S;
}

static void cfg_page_tick(void)
{
    static char room_cache[UI_CACHE_LEN], dim_cache[UI_CACHE_LEN];
    static char sleep_cache[UI_CACHE_LEN], ssid_cache[UI_CACHE_LEN];
    static char ip_cache[UI_CACHE_LEN], ha_cache[UI_CACHE_LEN], ver_cache[UI_CACHE_LEN];
    char buf[UI_CACHE_LEN];
    bool force = resync_due();

    snprintf(buf, sizeof(buf), "%s", rooms_count() ? rooms_name(rooms_current()) : "pièce ?");
    set_if_changed(CFG_ROOM, room_cache, buf, force);

    snprintf(buf, sizeof(buf), "%d %%", settings_brightness());
    set_if_changed(CFG_DIM_TXT, dim_cache, buf, force);

    if (settings_sleep_enabled())
        snprintf(buf, sizeof(buf), "%d s", settings_sleep_timeout_s());
    else
        snprintf(buf, sizeof(buf), "désactivée");
    set_if_changed(CFG_SLEEP_TXT, sleep_cache, buf, force);

    snprintf(buf, sizeof(buf), "%s", app_config_wifi_ssid());
    set_if_changed(CFG_SSID, ssid_cache, buf, force);

    char ip[16] = "--";
    if (!wifi_is_connected() || !wifi_get_ip(ip, sizeof(ip))) snprintf(ip, sizeof(ip), "--");
    set_if_changed(CFG_IP, ip_cache, ip, force);

    const char *ha;
    if (!app_config_ha_set()) ha = "non configuré";
    else if (ha_ws_is_ready()) ha = "connecté";
    else if (ha_ws_auth_invalid()) ha = "token refusé";
    else if (ha_ws_is_authed()) ha = "abonnement…";
    else ha = "connexion…";
    set_if_changed(CFG_HA, ha_cache, ha, force);

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (now_s < s_cfg_msg_until_s) {
        set_if_changed(CFG_VER, ver_cache, s_cfg_msg, false);
    } else {
        const esp_app_desc_t *app = esp_app_get_description();
        set_if_changed(CFG_VER, ver_cache, app->version, force);
    }

    if (s_cfg_needs_sync) {
        nx_set_val(CFG_DIM, settings_brightness());
        nx_set_val(CFG_SLEEP, settings_sleep_enabled() ? settings_sleep_timeout_s() : 0);
        s_cfg_needs_sync = false;
    }
}

static void ui_do_action(ui_action_t act)
{
    int32_t v;
    char ip[16];

    switch (act) {
    case ACT_ROOM_PREV:
    case ACT_ROOM_NEXT: {
        int n = rooms_count();
        if (n <= 0) break;
        int i = rooms_current() + (act == ACT_ROOM_NEXT ? 1 : -1);
        i = ((i % n) + n) % n;
        rooms_set_current(i);
        settings_save_default_room(rooms_name(i));
        break;
    }
    case ACT_DIM:
        if (nx_get_num(CFG_DIM, &v)) settings_save_brightness((int)v);
        break;
    case ACT_SLEEP:
        if (nx_get_num(CFG_SLEEP, &v)) {
            if (v <= 0) settings_save_sleep_enabled(0);
            else {
                settings_save_sleep_timeout((int)v);
                settings_save_sleep_enabled(1);
            }
        }
        break;
    case ACT_OTA:
    case ACT_TFT:
        /* Pas de clavier tactile pour une URL : la mise à jour se déclenche
         * depuis le portail web (POST /ota, /tft), qu'on rappelle ici. */
        if (wifi_is_connected() && wifi_get_ip(ip, sizeof(ip)))
            cfg_show_msg(ip);
        else
            cfg_show_msg("voir le portail web");
        break;
    case ACT_REBOOT:
        ESP_LOGI(TAG, "redémarrage demandé depuis cfg");
        vTaskDelay(pdMS_TO_TICKS(300));   /* laisse partir l'écriture NVS en cours */
        esp_restart();
        break;
    }
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
            if (now_s - s_last_touch_s >= ui_timeout_s()) {
                ESP_LOGI(TAG, "inactivité : retour à home");
                goto_page(NXP_HOME);
            }
        }

        if (s_page == NXP_HOME) home_page_tick(&tm);
        else if (s_page == NXP_CFG) cfg_page_tick();
    }
}

esp_err_t ui_start(void)
{
    /* 4 Ko : uniquement des tampons de quelques dizaines d'octets et snprintf. */
    return xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

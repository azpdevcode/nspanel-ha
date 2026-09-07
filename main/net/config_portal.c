/*
 * Portail web local de configuration du compte Home Assistant.
 *
 * Serveur HTTP minimal (esp_http_server) qui sert un formulaire « URL + token »
 * sur le LAN. Permet de COLLER le token longue durée (~180 caractères) depuis un
 * téléphone/PC au lieu de le taper sur l'écran tactile. À l'enregistrement, la
 * config est persistée (app_config) puis le panneau redémarre pour reconnecter
 * proprement le WebSocket HA.
 *
 * Sécurité : sert uniquement sur le réseau local (station, non exposé dehors).
 * Pas d'authentification (appareil domestique, réseau de confiance). Le token
 * courant n'est jamais renvoyé dans le formulaire (champ vide = inchangé).
 */
#include "config_portal.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "portal";

static httpd_handle_t s_server;

/* ---- Décodage application/x-www-form-urlencoded ---- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Décode `in` (percent-encoding + '+'→espace) dans `out` (NUL-terminé, tronqué
 * à cap-1). */
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

/* Coupe les espaces de tête/queue (in place). */
static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

/* ---- Pages ---- */

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=fr><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Configuration du panneau</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0d1117;color:#eef2f6;margin:0;padding:24px}"
    ".c{max-width:520px;margin:0 auto}h1{font-size:20px}label{display:block;margin:16px 0 6px;color:#8a93a2;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a313c;"
    "background:#161c24;color:#eef2f6;font-size:16px}"
    "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:10px;background:#3b82f6;"
    "color:#fff;font-size:16px;font-weight:600}.s{color:#34d399;font-size:14px;margin-top:8px}"
    "p.h{color:#5a636e;font-size:13px;margin-top:6px}</style></head><body><div class=c>";

static esp_err_t get_root(httpd_req_t *req)
{
    /* Buffer transitoire, libéré en fin de requête. */
    char *body = malloc(2560);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    snprintf(body, 2560,
        "%s"
        "<h1>Compte Home Assistant</h1>"
        "<form method=POST action=/save>"
        "<label>Adresse de votre serveur Home Assistant</label>"
        "<input name=ha_url value=\"%s\" placeholder=\"http://192.168.x.x:8123\">"
        "<p class=h>Adresse du serveur HA (PAS celle de ce panneau), avec le "
        "port, ex. http://192.168.50.89:8123</p>"
        "<label>Token longue dur&eacute;e</label>"
        "<input name=ha_token type=password placeholder=\"%s\">"
        "<p class=h>Profil HA &rarr; Jetons d'acc&egrave;s longue dur&eacute;e. "
        "Laisser vide pour conserver le token actuel.</p>"
        "<label>Broker MQTT (relais/boutons)</label>"
        "<input name=mqtt_uri value=\"%s\" placeholder=\"mqtt://192.168.x.x:1883\">"
        "<p class=h>Vide = relais/boutons non expos&eacute;s dans HA. "
        "Laisser vide pour d&eacute;sactiver.</p>"
        "<label>Utilisateur MQTT</label>"
        "<input name=mqtt_user value=\"%s\" placeholder=\"(optionnel)\">"
        "<label>Mot de passe MQTT</label>"
        "<input name=mqtt_pass type=password placeholder=\"%s\">"
        "<p class=h>Laisser vide pour conserver le mot de passe actuel.</p>"
        "<button type=submit>Enregistrer &amp; red&eacute;marrer</button>"
        "</form></div></body></html>",
        PAGE_HEAD, app_config_ha_url(),
        app_config_ha_token()[0] ? "token enregistre - coller pour le remplacer"
                                 : "coller le token ici",
        app_config_mqtt_uri(), app_config_mqtt_user(),
        app_config_mqtt_pass()[0] ? "mot de passe enregistre - coller pour le remplacer"
                                  : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return r;
}

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Redémarrage après configuration HA");
    esp_restart();
}

/* Tailles des sous-buffers (cf. découpage du bloc PSRAM ci-dessous). */
#define P_RAW   2048
#define P_UENC  (APP_CFG_URL_MAX * 3)
#define P_TENC  (APP_CFG_TOKEN_MAX * 3)
#define P_URL   APP_CFG_URL_MAX
#define P_TOK   APP_CFG_TOKEN_MAX
#define P_MU_ENC (APP_CFG_MQTT_URI_MAX * 3)
#define P_MN_ENC (APP_CFG_MQTT_USER_MAX * 3)
#define P_MP_ENC (APP_CFG_MQTT_PASS_MAX * 3)
#define P_MU     APP_CFG_MQTT_URI_MAX
#define P_MN     APP_CFG_MQTT_USER_MAX
#define P_MP     APP_CFG_MQTT_PASS_MAX
#define P_BODY  1280
#define P_TOTAL (P_RAW + P_UENC + P_TENC + P_URL + P_TOK + \
                P_MU_ENC + P_MN_ENC + P_MP_ENC + P_MU + P_MN + P_MP + P_BODY)

static esp_err_t post_save(httpd_req_t *req)
{
    /* Tous les buffers de travail dans UN bloc, libéré en fin de requête. */
    char *mem = malloc(P_TOTAL);
    if (!mem) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    char *raw     = mem;
    char *url_enc = raw + P_RAW;
    char *tok_enc = url_enc + P_UENC;
    char *mu_enc  = tok_enc + P_TENC;
    char *mn_enc  = mu_enc + P_MU_ENC;
    char *mp_enc  = mn_enc + P_MN_ENC;
    char *url     = mp_enc + P_MP_ENC;
    char *token   = url + P_URL;
    char *mqtt_uri  = token + P_TOK;
    char *mqtt_user = mqtt_uri + P_MU;
    char *mqtt_pass = mqtt_user + P_MN;
    char *body      = mqtt_pass + P_MP;

    /* Corps form-urlencoded : ha_url=...&ha_token=... */
    int total = req->content_len;
    if (total <= 0 || total >= P_RAW) {
        free(mem);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corps invalide");
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, raw + received, total - received);
        if (r <= 0) {
            free(mem);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "lecture");
        }
        received += r;
    }
    raw[total] = '\0';

    url[0] = token[0] = mqtt_uri[0] = mqtt_user[0] = mqtt_pass[0] = '\0';
    if (httpd_query_key_value(raw, "ha_url", url_enc, P_UENC) == ESP_OK)
        url_decode(url_enc, url, P_URL);
    if (httpd_query_key_value(raw, "ha_token", tok_enc, P_TENC) == ESP_OK)
        url_decode(tok_enc, token, P_TOK);
    if (httpd_query_key_value(raw, "mqtt_uri", mu_enc, P_MU_ENC) == ESP_OK)
        url_decode(mu_enc, mqtt_uri, P_MU);
    if (httpd_query_key_value(raw, "mqtt_user", mn_enc, P_MN_ENC) == ESP_OK)
        url_decode(mn_enc, mqtt_user, P_MN);
    if (httpd_query_key_value(raw, "mqtt_pass", mp_enc, P_MP_ENC) == ESP_OK)
        url_decode(mp_enc, mqtt_pass, P_MP);
    trim(url);
    trim(token);
    trim(mqtt_uri);
    trim(mqtt_user);
    trim(mqtt_pass);

    /* Mot de passe MQTT vide → conserver l'actuel (même logique que le token HA). */
    if (mqtt_pass[0] == '\0') {
        strncpy(mqtt_pass, app_config_mqtt_pass(), P_MP - 1);
        mqtt_pass[P_MP - 1] = '\0';
    }

    /* Token vide → conserver l'actuel (le formulaire ne le renvoie pas). */
    if (token[0] == '\0') {
        strncpy(token, app_config_ha_token(), P_TOK - 1);
        token[P_TOK - 1] = '\0';
    }

    if (url[0] == '\0') {
        free(mem);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL manquante");
    }

    esp_err_t err = app_config_set_ha(url, token);
    if (err != ESP_OK) {
        free(mem);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "enregistrement");
    }
    /* Non fatal : le compte HA est la valeur qui compte le plus, MQTT reste optionnel. */
    if (app_config_set_mqtt(mqtt_uri, mqtt_user, mqtt_pass) != ESP_OK)
        ESP_LOGW(TAG, "échec enregistrement config MQTT");

    /* url pointe dans `mem` comme body → on relit la valeur depuis le cache
     * app_config (objet distinct) pour éviter un chevauchement (-Wrestrict). */
    snprintf(body, P_BODY,
        "%s<h1>Enregistr&eacute; ✓</h1>"
        "<p class=s>Le panneau red&eacute;marre pour se connecter &agrave; %s.</p>"
        "<p class=h>V&eacute;rifier ensuite <code>GET /status</code> &rarr; "
        "<code>ha.ws_authed</code> doit passer &agrave; <code>true</code>, "
        "puis <code>ha.rooms</code> se peuple en quelques secondes.</p>"
        "</div></body></html>", PAGE_HEAD, app_config_ha_url());
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(mem);

    /* Redémarrage différé : laisse la réponse HTTP partir avant le reset. */
    const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "portal_restart" };
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK)
        esp_timer_start_once(t, 1500 * 1000);   /* 1,5 s */
    return ESP_OK;
}

esp_err_t config_portal_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    /* Portail léger : peu de connexions simultanées → libère des sockets/fds
     * LWIP (le défaut 7 réserve quasi tout le pool) et de la RAM. */
    cfg.max_open_sockets = 3;
    /* Défaut = 8 : /save (ce fichier) + les routes de dev_portal (bridge, tft,
     * status, nx, scan, ota, rooms) + le catch-all racine dépassent ce compte.
     * Un handler enregistré au-delà du max échoue SILENCIEUSEMENT côté appelant
     * si l'erreur n'est pas vérifiée — c'est arrivé ici : l'ajout de /rooms a
     * fait déborder le catch-all « / » (le dernier enregistré), laissant le
     * portail répondre « nothing match the given url » sur la racine. Marge
     * volontaire au-delà du compte actuel pour ne pas rejouer ce bug au
     * prochain endpoint ajouté. */
    cfg.max_uri_handlers = 16;
    /* Matcher wildcard : tout chemin GET (ex. http://<ip>/<token> tapé par
     * erreur dans la barre d'adresse) sert le formulaire au lieu d'un 404. */
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    httpd_register_uri_handler(s_server, &save);
    /* Le catch-all GET wildcard n'est PAS enregistré ici : avec le matcher
     * wildcard, il capterait toute requête GET enregistrée après lui (dont
     * /status côté dev_portal). Voir config_portal_register_fallback(). */
    ESP_LOGI(TAG, "Portail de configuration prêt (http://<ip>/)");
    return ESP_OK;
}

httpd_handle_t config_portal_handle(void) { return s_server; }

esp_err_t config_portal_register_fallback(void)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    /* Enregistré en dernier, après toutes les routes spécifiques (dev_portal
     * incluse) : avec le matcher wildcard, l'ordre d'enregistrement fait office
     * de priorité, et ce catch-all doit perdre face à un chemin exact. */
    httpd_uri_t root = { .uri = "/*", .method = HTTP_GET, .handler = get_root };
    return httpd_register_uri_handler(s_server, &root);
}

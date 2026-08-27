/*
 * Pilote Nextion — UART2 (TX=GPIO16, RX=GPIO17 côté ESP32 vers l'écran STM32G0).
 * Voir nextion.h pour le contrat. Détails du protocole : commandes ASCII
 * terminées par 0xFF 0xFF 0xFF ; retours binaires préfixés par un code
 * (0x65 tactile, 0x66 page courante, 0x70/0x71 réponse à `get`, 0x88 prêt,
 * 0x87 réveil tactile, 0x1A/0x1B.. erreurs).
 */
#include "nextion.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"

static const char *TAG = "nextion";

#define NX_UART_NUM     UART_NUM_2
#define NX_TX_PIN       16
#define NX_RX_PIN       17
#define NX_BAUD_BOOT    115200
#define NX_BAUD_FAST    921600
#define NX_RX_BUF_SIZE  1024

#define NX_NVS_NS       "nspanel"

/* ---- Tables générées ---- */

static const char *const s_page_name[NXP_COUNT] = {
#define NX_PAGE(id, name) [id] = name,
#include "nx_pages.def"
#undef NX_PAGE
};

typedef struct { nx_page_t page; const char *name; const char *attr; } nx_obj_meta_t;

static const nx_obj_meta_t s_obj_meta[NX_OBJ_COUNT] = {
#define NX_OBJ(id, page, name, attr) [id] = { page, name, attr },
#include "nx_objects.def"
#undef NX_OBJ
};

static bool      s_present[NX_OBJ_COUNT];
static nx_obj_t  s_page_anchor[NXP_COUNT];

static QueueHandle_t s_touch_q;
static TaskHandle_t  s_rx_task;
static bool           s_have_spec;
static int32_t        s_spec_rev = -1;

/* Réveil tactile pendant le sommeil écran (0x87) : événement synthétique posté
 * sur la file tactile pour que ui_worker sorte de veille et rafraîchisse. */
#define NX_WAKE_PAGE 0xFFu
#define NX_WAKE_CID  0xFFu

/* ---- Bas niveau UART ---- */

static void uart_send_raw(const void *data, size_t len)
{
    uart_write_bytes(NX_UART_NUM, data, len);
}

static void send_terminator(void)
{
    static const uint8_t term[3] = { 0xFF, 0xFF, 0xFF };
    uart_send_raw(term, sizeof(term));
}

void nx_cmd(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    uart_send_raw(buf, (size_t)n);
    send_terminator();
}

/* Échappe " et \ pour une insertion sûre dans <obj>.txt="...". */
static void escape_into(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

/* ---- Lecture de trame (utilisée pour le sondage ET la tâche RX) ----
 * Retourne true si une trame complète (hors terminateur) a été lue dans buf/len,
 * dans le budget timeout_ms. Le terminateur 0xFF 0xFF 0xFF n'est PAS inclus dans buf. */
static bool read_frame(uint8_t *buf, size_t cap, size_t *len, uint32_t timeout_ms)
{
    size_t n = 0;
    int ff_run = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t b;
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) break;
        TickType_t wait = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (wait == 0) wait = 1;
        int r = uart_read_bytes(NX_UART_NUM, &b, 1, wait);
        if (r <= 0) continue;
        if (b == 0xFF) {
            ff_run++;
            if (ff_run >= 3) { *len = n; return true; }
            continue;
        }
        /* Un 0xFF isolé (1 ou 2) suivi d'un octet non-0xFF fait partie de la charge
         * utile (rare, mais le protocole ne l'interdit pas côté string returns) :
         * on le réinjecte dans le tampon avant l'octet courant. */
        while (ff_run > 0 && n < cap) { buf[n++] = 0xFF; ff_run--; }
        ff_run = 0;
        if (n < cap) buf[n++] = b;
    }
    return false;
}

/* ---- Sondage synchrone (RX task suspendue) ---- */

typedef struct {
    bool    ok;
    bool    is_num;
    int32_t num;
} nx_get_result_t;

static bool get_sync(const char *name, const char *attr, nx_get_result_t *res)
{
    memset(res, 0, sizeof(*res));
    uart_flush_input(NX_UART_NUM);
    nx_cmd("get %s.%s", name, attr);

    uint8_t frame[16];
    size_t flen = 0;
    if (!read_frame(frame, sizeof(frame), &flen, 300)) {
        ESP_LOGD(TAG, "get %s.%s: pas de réponse (timeout)", name, attr);
        return false;
    }
    if (flen == 0) return false;
    switch (frame[0]) {
    case 0x71: /* numérique : 4 octets little-endian après le code */
        if (flen >= 5) {
            res->is_num = true;
            res->num = (int32_t)(frame[1] | (frame[2] << 8) | (frame[3] << 16) | (frame[4] << 24));
        }
        res->ok = true;
        return true;
    case 0x70: /* chaîne : présence confirmée, contenu ignoré ici */
        res->ok = true;
        return true;
    case 0x1A:
        ESP_LOGD(TAG, "get %s.%s: composant/attribut inconnu (0x1A)", name, attr);
        return false;
    default:
        ESP_LOGD(TAG, "get %s.%s: code retour inattendu 0x%02X", name, attr, frame[0]);
        return false;
    }
}

/* ---- NVS : cache du bitmap de présence, keyé par révision vSpec ---- */

static void nvs_load_cache(int32_t rev)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t cached_rev = -1;
    nvs_get_i32(h, "nx_rev", &cached_rev);
    if (cached_rev == rev) {
        size_t blen = sizeof(s_present);
        if (nvs_get_blob(h, "nx_bits", s_present, &blen) == ESP_OK && blen == sizeof(s_present)) {
            ESP_LOGI(TAG, "bitmap de présence rechargé du cache NVS (rev %ld)", (long)rev);
            nvs_close(h);
            return;
        }
    }
    nvs_close(h);
    /* Cache absent/désaccordé : laisser l'appelant décider de re-sonder. */
}

static void nvs_save_cache(int32_t rev)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "nx_rev", rev);
    nvs_set_blob(h, "nx_bits", s_present, sizeof(s_present));
    nvs_commit(h);
    nvs_close(h);
}

static bool cache_matches(int32_t rev)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t cached_rev = -1;
    nvs_get_i32(h, "nx_rev", &cached_rev);
    nvs_close(h);
    return cached_rev == rev;
}

void nextion_force_reprobe(void)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "nx_rev");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- Sondage complet ---- */

void nextion_probe(void)
{
    if (s_rx_task) vTaskSuspend(s_rx_task);

    for (int i = 0; i < NXP_COUNT; i++) s_page_anchor[i] = NX_OBJ_COUNT;
    memset(s_present, 0, sizeof(s_present));

    nx_get_result_t r;
    s_have_spec = get_sync("vSpec", "val", &r) && r.is_num;
    s_spec_rev = s_have_spec ? r.num : -1;

    if (!s_have_spec) {
        ESP_LOGW(TAG, "vSpec absente — écran d'origine ou HMI non conforme, "
                       "tous les objets restent marqués absents");
        if (s_rx_task) vTaskResume(s_rx_task);
        return;
    }

    if (cache_matches(s_spec_rev)) {
        nvs_load_cache(s_spec_rev);
    } else {
        ESP_LOGI(TAG, "sondage complet du .tft (vSpec rev %ld, %d objets)…",
                 (long)s_spec_rev, NX_OBJ_COUNT);
        int found = 0;
        for (int i = 0; i < NX_OBJ_COUNT; i++) {
            s_present[i] = get_sync(s_obj_meta[i].name, s_obj_meta[i].attr, &r);
            if (s_present[i]) found++;
        }
        ESP_LOGI(TAG, "sondage terminé : %d/%d objets présents", found, NX_OBJ_COUNT);
        nvs_save_cache(s_spec_rev);
    }

    /* Ancre = premier objet déclaré pour chaque page dans nx_objects.def. */
    for (int i = 0; i < NX_OBJ_COUNT; i++) {
        nx_page_t p = s_obj_meta[i].page;
        if (s_page_anchor[p] == NX_OBJ_COUNT) s_page_anchor[p] = (nx_obj_t)i;
    }

    if (s_rx_task) vTaskResume(s_rx_task);
}

bool nx_present(nx_obj_t o)
{
    if (o < 0 || o >= NX_OBJ_COUNT) return false;
    return s_present[o];
}

bool nx_page_present(nx_page_t p)
{
    if (p < 0 || p >= NXP_COUNT) return false;
    nx_obj_t a = s_page_anchor[p];
    if (a == NX_OBJ_COUNT) return false;
    return s_present[a];
}

/* ---- Écritures tolérantes ---- */

bool nx_set_txt(nx_obj_t o, const char *s)
{
    if (!nx_present(o)) return false;
    char esc[192];
    escape_into(s ? s : "", esc, sizeof(esc));
    nx_cmd("%s.txt=\"%s\"", s_obj_meta[o].name, esc);
    return true;
}

bool nx_set_val(nx_obj_t o, int32_t v)
{
    if (!nx_present(o)) return false;
    nx_cmd("%s.val=%ld", s_obj_meta[o].name, (long)v);
    return true;
}

bool nx_set_pco(nx_obj_t o, uint16_t rgb565)
{
    if (!nx_present(o)) return false;
    nx_cmd("%s.pco=%u", s_obj_meta[o].name, (unsigned)rgb565);
    return true;
}

bool nx_vis(nx_obj_t o, bool on)
{
    if (!nx_present(o)) return false;
    nx_cmd("vis %s,%d", s_obj_meta[o].name, on ? 1 : 0);
    return true;
}

bool nx_page(nx_page_t p)
{
    if (p < 0 || p >= NXP_COUNT) return false;
    if (!nx_page_present(p)) {
        ESP_LOGW(TAG, "nx_page(%s) ignoré : page absente du .tft", s_page_name[p]);
        return false;
    }
    nx_cmd("page %s", s_page_name[p]);
    return true;
}

void nx_dim(uint8_t pct)
{
    if (pct > 100) pct = 100;
    nx_cmd("dim=%u", (unsigned)pct);
}

/* ---- Tâche RX : événements tactiles + veille ---- */

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t frame[16];
    size_t flen;
    for (;;) {
        if (!read_frame(frame, sizeof(frame), &flen, portMAX_DELAY)) continue;
        if (flen == 0) continue;
        switch (frame[0]) {
        case 0x65: /* touch : 0x65 page cid event */
            if (flen >= 4) {
                nx_touch_t t = { .page = frame[1], .cid = frame[2], .pressed = frame[3] };
                xQueueSend(s_touch_q, &t, 0);
            }
            break;
        case 0x87: { /* réveil tactile pendant le sommeil écran */
            nx_touch_t t = { .page = NX_WAKE_PAGE, .cid = NX_WAKE_CID, .pressed = 1 };
            xQueueSend(s_touch_q, &t, 0);
            break;
        }
        case 0x88:
            ESP_LOGI(TAG, "écran prêt (0x88)");
            break;
        case 0x1A:
        case 0x1B:
            ESP_LOGW(TAG, "erreur écran inattendue (0x%02X) hors sondage", frame[0]);
            break;
        default:
            ESP_LOGD(TAG, "trame ignorée (code 0x%02X, %u octets)", frame[0], (unsigned)flen);
            break;
        }
    }
}

bool nextion_get_touch(nx_touch_t *out, uint32_t timeout_ms)
{
    if (!s_touch_q) return false;
    TickType_t wait = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_touch_q, out, wait) == pdTRUE;
}

/* ---- Négociation de débit + init ---- */

/* Sortie du « Protocol Reparse Mode ».
 *
 * Le firmware Sonoff d'origine laisse l'écran en reparse actif : il n'interprète
 * alors PAS les commandes Nextion standard (`get`, `page`, `obj.txt=`), mais un
 * protocole JSON propriétaire. Tant qu'on n'en est pas sorti, l'écran paraît
 * muet et un diagnostic de câblage serait un faux positif.
 *
 * La séquence d'échappement documentée est la chaîne magique ci-dessous, suivie
 * de deux `recmod=0` (le doublon est recommandé par l'implémentation ESPHome de
 * référence : le premier est parfois consommé par le parseur en cours de bascule).
 * Sans effet sur un écran déjà en mode passif — donc sûr à envoyer à chaque boot,
 * y compris une fois notre propre .tft chargé. */
static void exit_reparse_mode(void)
{
    nx_cmd("DRAKJHSUYDGBNCJHGJKSHBDN");
    vTaskDelay(pdMS_TO_TICKS(50));
    nx_cmd("recmod=0");
    nx_cmd("recmod=0");
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input(NX_UART_NUM);
}

static bool try_baud(uint32_t baud, uint32_t verify_timeout_ms)
{
    (void)verify_timeout_ms;
    uart_set_baudrate(NX_UART_NUM, baud);
    uart_flush_input(NX_UART_NUM);
    nx_get_result_t r;
    /* `get sleep` : présent sur tout firmware Nextion, sert juste à vérifier
     * qu'on se comprend au débit courant. */
    if (get_sync("sleep", "val", &r)) return true;
    /* Pas de réponse : peut-être un reparse actif plutôt qu'un mauvais débit. */
    exit_reparse_mode();
    return get_sync("sleep", "val", &r);
}

esp_err_t nextion_init(void)
{
    uart_config_t cfg = {
        .baud_rate = NX_BAUD_BOOT,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(NX_UART_NUM, NX_RX_BUF_SIZE, NX_RX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    uart_param_config(NX_UART_NUM, &cfg);
    uart_set_pin(NX_UART_NUM, NX_TX_PIN, NX_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* Le STM32 de l'écran met ~500 ms à démarrer après la mise sous tension de
     * GPIO4 (nspanel_io_init, appelé juste avant). Le sonder trop tôt le fait
     * passer pour absent. */
    vTaskDelay(pdMS_TO_TICKS(600));

    /* Écran d'origine = reparse actif : il ignore les commandes standard tant
     * qu'on n'en est pas sorti. À faire AVANT toute négociation de débit, sinon
     * `baud=` lui-même est ignoré. Sans effet si déjà en mode passif. */
    exit_reparse_mode();

    /* Négociation : on tente de monter à 921600 ; repli silencieux sur 115200
     * si l'écran ne répond pas (clones/anciens firmwares qui ignorent `baud=`). */
    uart_flush_input(NX_UART_NUM);
    nx_cmd("bkcmd=2");         /* défaut : ne renvoie que les échecs — silencieux au succès */
    nx_cmd("baud=%d", NX_BAUD_FAST);  /* volatile : non gravé, sûr si l'ESP32 replante */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (try_baud(NX_BAUD_FAST, 300)) {
        ESP_LOGI(TAG, "écran Nextion à %d bauds", NX_BAUD_FAST);
    } else {
        ESP_LOGW(TAG, "pas de réponse à %d bauds, repli sur %d", NX_BAUD_FAST, NX_BAUD_BOOT);
        if (!try_baud(NX_BAUD_BOOT, 300)) {
            ESP_LOGW(TAG, "aucune réponse de l'écran (TX=%d RX=%d). Causes possibles, "
                          "dans l'ordre : reparse mode non quitté, écran encore en "
                          "démarrage, câblage.", NX_TX_PIN, NX_RX_PIN);
        }
    }

    s_touch_q = xQueueCreate(8, sizeof(nx_touch_t));
    if (!s_touch_q) return ESP_ERR_NO_MEM;

    nextion_probe();

    if (xTaskCreate(rx_task, "nx_rx", 3072, NULL, 5, &s_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(nx_rx) échoué");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Pont série transparent (upload TFT via Nextion Editor) ---- */

void nextion_bridge_run(void)
{
    if (s_rx_task) vTaskSuspend(s_rx_task);
    ESP_LOGW(TAG, "pont série actif : USB <-> écran, redémarrage requis pour en sortir");
    uart_set_baudrate(NX_UART_NUM, NX_BAUD_BOOT);

    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) uart_write_bytes(NX_UART_NUM, buf, n);
        n = uart_read_bytes(NX_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) uart_write_bytes(UART_NUM_0, buf, n);
    }
}

/* ---- Upload TFT-OTA ---- */

/* Un seul octet 0x05 (sans terminateur FFFFFF) = accusé d'un bloc/de la demande. */
static bool wait_ack_byte(uint32_t timeout_ms)
{
    uint8_t b;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        TickType_t wait = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (wait == 0) break;
        int r = uart_read_bytes(NX_UART_NUM, &b, 1, wait);
        if (r <= 0) continue;
        if (b == 0x05) return true;
        ESP_LOGW(TAG, "upload TFT: octet inattendu 0x%02X en attente d'accusé", b);
    }
    return false;
}

bool nx_tft_upload_begin(uint32_t size_bytes, uint32_t baud, uint32_t timeout_ms)
{
    if (s_rx_task) vTaskSuspend(s_rx_task);
    uart_set_baudrate(NX_UART_NUM, baud);
    uart_flush_input(NX_UART_NUM);
    nx_cmd("whmi-wri=%u,%u,0", (unsigned)size_bytes, (unsigned)baud);
    bool ok = wait_ack_byte(timeout_ms);
    if (!ok) ESP_LOGE(TAG, "upload TFT: pas d'accusé initial (whmi-wri)");
    return ok;
}

bool nx_tft_upload_chunk(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uart_write_bytes(NX_UART_NUM, data, len);
    return wait_ack_byte(timeout_ms);
}

void nx_tft_upload_end(void)
{
    /* L'écran redémarre seul après le dernier bloc accusé ; on renégocie notre
     * débit de croisière et on force un re-sondage (nouveau .tft = nouveau vSpec). */
    vTaskDelay(pdMS_TO_TICKS(500));
    nextion_force_reprobe();
    if (!try_baud(NX_BAUD_FAST, 300)) {
        try_baud(NX_BAUD_BOOT, 300);
    }
    nextion_probe();   /* suspend/resume la tâche RX elle-même */
}

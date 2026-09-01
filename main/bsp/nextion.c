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
#include "freertos/semphr.h"
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

/* Pause coopérative de rx_task, PAS vTaskSuspend() : rx_task lit l'UART avec un
 * timeout borné (voir rx_task()) et vérifie ce drapeau entre deux lectures, au
 * lieu de bloquer indéfiniment dans uart_read_bytes(). Suspendre une tâche en
 * plein appel bloquant ne l'annule pas proprement côté driver UART — elle reste
 * inscrite comme lecteur en attente dans le ring buffer, et un uart_flush_input()
 * ou uart_write_bytes() appelé depuis une autre tâche pendant ce temps peut se
 * bloquer à son tour (constaté : /status figé indéfiniment sur "flush_input"
 * pendant un upload TFT-OTA). */
static volatile bool s_rx_paused;

static void rx_pause(void)
{
    if (!s_rx_task) return;
    s_rx_paused = true;
    vTaskDelay(pdMS_TO_TICKS(120));   /* laisse rx_task sortir de son read borné */
}

static void rx_resume(void)
{
    s_rx_paused = false;
}

/* Réveil tactile pendant le sommeil écran (0x87) : événement synthétique posté
 * sur la file tactile pour que ui_worker sorte de veille et rafraîchisse. */
#define NX_WAKE_PAGE 0xFFu
#define NX_WAKE_CID  0xFFu

/* ---- Bas niveau UART ---- */

/* ---- Verrou du bus écran ----
 *
 * L'UART écran est écrit par plusieurs tâches : la boucle applicative (statut
 * boot toutes les 2 s), le serveur HTTP (/nx, /scan) et la tâche TFT-OTA. Sans
 * verrou, une écriture de 50 octets s'insère au milieu d'un bloc de 4 Ko du
 * flux .tft : l'écran continue d'accuser ses blocs — le compte d'octets reste
 * juste — mais écrit un .tft décalé, et affiche « System data ERROR! » au
 * redémarrage. Transfert « réussi », écran mort ; c'est exactement le symptôme
 * qui a coûté plusieurs heures.
 *
 * Récursif : nx_tft_upload_*() et nextion_probe() prennent le verrou puis
 * appellent nx_cmd()/get_sync() qui le reprennent depuis la même tâche.
 *
 * Les écritures d'UI sont des rafraîchissements idempotents : plutôt que de
 * bloquer la boucle applicative pendant les 45 s d'un upload, elles ABANDONNENT
 * si le bus est pris (voir NX_BUS_WAIT_MS). La prochaine passe les rejouera. */
static SemaphoreHandle_t s_bus_lock;

#define NX_BUS_WAIT_MS 50

bool nx_bus_acquire(uint32_t timeout_ms)
{
    if (!s_bus_lock) return true;   /* avant nextion_init() : mono-tâche */
    return xSemaphoreTakeRecursive(s_bus_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void nx_bus_release(void)
{
    if (s_bus_lock) xSemaphoreGiveRecursive(s_bus_lock);
}

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
    if (!nx_bus_acquire(NX_BUS_WAIT_MS)) {
        ESP_LOGD(TAG, "bus occupé, commande abandonnée : %.40s", buf);
        return;
    }
    uart_send_raw(buf, (size_t)n);
    send_terminator();
    nx_bus_release();
}

/* Transcode UTF-8 -> Latin-1 et échappe " et \ pour <obj>.txt="...".
 *
 * Les polices .zi du projet sont générées en iso-8859-1, un octet par
 * caractère : le mode utf-8 du générateur Nextion produit une table couvrant
 * les 65 536 codes du BMP, soit ~670 Ko par police au lieu de quelques
 * dizaines (voir design/HMI-SPEC.md). Home Assistant, lui, renvoie de
 * l'UTF-8 — d'où cette conversion, sans quoi « Éclairage » et « 21.5° »
 * arriveraient en mojibake.
 *
 * Tout ce qui sort de Latin-1 devient '?', y compris U+00FF : son octet 0xFF
 * serait pris pour le début d'un terminateur FF FF FF. */
static void latin1_escape_into(const char *in, char *out, size_t cap)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t o = 0;

    while (*p && o + 2 < cap) {
        uint8_t c = *p;
        uint32_t cp;

        /* Les && court-circuitent : p[1]/p[2]/p[3] ne sont lus que si l'octet
         * précédent était une continuation, donc jamais au-delà du '\0'. */
        if (c < 0x80) {
            cp = c;
            p += 1;
        } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = 0x100;                       /* hors Latin-1 par construction */
            p += 3;
        } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = 0x100;
            p += 4;
        } else {
            cp = 0x100;                       /* octet isolé / séquence cassée */
            p += 1;
        }

        char ch = (cp < 0xFF) ? (char)cp : '?';
        if (ch == '"' || ch == '\\') out[o++] = '\\';
        out[o++] = ch;
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

/* attr == NULL : variable SYSTÈME (sleep, dim, baud…), dont la syntaxe est
 * `get <nom>` sans suffixe — `get sleep.val` est invalide et reste sans réponse,
 * quel que soit le débit. */
/* Nom pleinement qualifié `<page>.<objet>`.
 *
 * Nextion n'expose un composant sous son nom nu que pendant que sa page est
 * affichée ; hors de là, `get tClock.txt` répond 0x1A même si l'objet existe.
 * Le préfixe de page lève cette restriction en lecture comme en écriture, et la
 * valeur écrite survit à la navigation — vérifié sur la cible. C'est ce qui
 * dispense de mettre chaque composant en `vscope=global` dans l'Editor : la
 * page est déjà déclarée dans nx_objects.def, donc la contrainte disparaît du
 * côté humain, là où elle était systématiquement oubliée. */
static const char *qname(nx_obj_t o, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s.%s", s_page_name[s_obj_meta[o].page], s_obj_meta[o].name);
    return buf;
}

static bool get_sync(const char *name, const char *attr, nx_get_result_t *res)
{
    memset(res, 0, sizeof(*res));
    /* Écriture ET lecture sous le même verrou : sans ça une commande d'une
     * autre tâche s'intercale et c'est SA réponse qu'on lirait. Un seul point
     * de sortie pour ne pas risquer d'oublier le nx_bus_release(). */
    if (!nx_bus_acquire(NX_BUS_WAIT_MS)) return false;

    uart_flush_input(NX_UART_NUM);
    if (attr) nx_cmd("get %s.%s", name, attr);
    else      nx_cmd("get %s", name);

    uint8_t frame[16];
    size_t flen = 0;
    bool ok = false;

    if (!read_frame(frame, sizeof(frame), &flen, 300)) {
        ESP_LOGD(TAG, "get %s%s%s: pas de réponse (timeout)",
                 name, attr ? "." : "", attr ? attr : "");
    } else if (flen > 0) {
        switch (frame[0]) {
        case 0x71: /* numérique : 4 octets little-endian après le code */
            if (flen >= 5) {
                res->is_num = true;
                res->num = (int32_t)(frame[1] | (frame[2] << 8) |
                                     (frame[3] << 16) | (frame[4] << 24));
            }
            res->ok = true;
            ok = true;
            break;
        case 0x70: /* chaîne : présence confirmée, contenu ignoré ici */
            res->ok = true;
            ok = true;
            break;
        case 0x1A:
            ESP_LOGD(TAG, "get %s%s%s: composant/attribut inconnu (0x1A)",
                     name, attr ? "." : "", attr ? attr : "");
            break;
        default:
            ESP_LOGD(TAG, "get %s%s%s: code retour inattendu 0x%02X",
                     name, attr ? "." : "", attr ? attr : "", frame[0]);
            break;
        }
    }

    nx_bus_release();
    return ok;
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
    /* 68 allers-retours : sans le verrou tenu de bout en bout, une écriture
     * d'UI intercalée fausserait la réponse lue pour l'objet en cours. */
    if (!nx_bus_acquire(5000)) {
        ESP_LOGW(TAG, "sondage annulé : bus écran occupé");
        return;
    }
    rx_pause();

    for (int i = 0; i < NXP_COUNT; i++) s_page_anchor[i] = NX_OBJ_COUNT;
    memset(s_present, 0, sizeof(s_present));

    nx_get_result_t r;
    char vspec_q[40];
    snprintf(vspec_q, sizeof(vspec_q), "%s.vSpec", s_page_name[NXP_BOOT]);
    s_have_spec = get_sync(vspec_q, "val", &r) && r.is_num;
    s_spec_rev = s_have_spec ? r.num : -1;

    if (!s_have_spec) {
        ESP_LOGW(TAG, "vSpec absente — écran d'origine ou HMI non conforme, "
                       "tous les objets restent marqués absents");
        rx_resume();
        nx_bus_release();
        return;
    }

    if (cache_matches(s_spec_rev)) {
        nvs_load_cache(s_spec_rev);
    } else {
        ESP_LOGI(TAG, "sondage complet du .tft (vSpec rev %ld, %d objets)…",
                 (long)s_spec_rev, NX_OBJ_COUNT);
        int found = 0;
        for (int i = 0; i < NX_OBJ_COUNT; i++) {
            char q[40];
            s_present[i] = get_sync(qname((nx_obj_t)i, q, sizeof(q)),
                                    s_obj_meta[i].attr, &r);
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

    rx_resume();
    nx_bus_release();
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

bool nextion_spec_present(void) { return s_have_spec; }
int32_t nextion_spec_revision(void) { return s_spec_rev; }

void nextion_objects_summary(int *present, int *total)
{
    int found = 0;
    for (int i = 0; i < NX_OBJ_COUNT; i++) if (s_present[i]) found++;
    if (present) *present = found;
    if (total) *total = NX_OBJ_COUNT;
}

/* ---- Écritures tolérantes ---- */

bool nx_set_txt(nx_obj_t o, const char *s)
{
    if (!nx_present(o)) return false;
    char esc[192], q[40];
    latin1_escape_into(s ? s : "", esc, sizeof(esc));
    nx_cmd("%s.txt=\"%s\"", qname(o, q, sizeof(q)), esc);
    return true;
}

/* Écrit sur l'attribut numérique DÉCLARÉ dans nx_objects.def, pas sur `.val` en
 * dur : une Progress bar expose `val`, un Picture `pic`. C'est déjà l'attribut
 * que nextion_probe() a sondé, donc présence et écriture visent la même case. */
bool nx_set_val(nx_obj_t o, int32_t v)
{
    if (!nx_present(o)) return false;
    char q[40];
    nx_cmd("%s.%s=%ld", qname(o, q, sizeof(q)), s_obj_meta[o].attr, (long)v);
    return true;
}

bool nx_set_pco(nx_obj_t o, uint16_t rgb565)
{
    if (!nx_present(o)) return false;
    char q[40];
    nx_cmd("%s.pco=%u", qname(o, q, sizeof(q)), (unsigned)rgb565);
    return true;
}

bool nx_vis(nx_obj_t o, bool on)
{
    if (!nx_present(o)) return false;
    /* Seule écriture qui reste en nom nu : `vis` refuse un nom qualifié
     * (répond 0x02, « nom de composant invalide »). Elle n'a donc d'effet que
     * si la page de l'objet est affichée — ce qui est aussi le seul moment où
     * une bascule de visibilité se voit. */
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

#define NX_RX_POLL_MS  100   /* borne le blocage dans uart_read_bytes() pour que
                                * la pause coopérative (s_rx_paused) reste réactive */

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t frame[16];
    size_t flen;
    for (;;) {
        if (s_rx_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!read_frame(frame, sizeof(frame), &flen, NX_RX_POLL_MS)) continue;
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
    /* `get sleep` : variable SYSTÈME, donc SANS suffixe `.val` (attr = NULL).
     * `get sleep.val` est invalide et ne répond jamais — c'était le bug qui
     * faisait échouer la détection de débit à tous les débits, alors que la même
     * requête bien formée répondait `71 00 00 00 00 FF FF FF`. */
    if (get_sync("sleep", NULL, &r)) return true;
    /* Pas de réponse : peut-être un reparse actif plutôt qu'un mauvais débit. */
    exit_reparse_mode();
    return get_sync("sleep", NULL, &r);
}

/* Débit auquel l'écran a répondu pour la dernière fois. Un .tft embarque SON
 * propre réglage de débit (un projet Nextion neuf est souvent à 9600), et il
 * change donc à chaque upload : le supposer en dur condamne le pilote à devenir
 * muet dès qu'on téléverse un .tft qui n'est pas à 115200. */
static uint32_t s_baud_current;

/* 115200 en tête (valeur du firmware Sonoff d'origine ET de la config ESPHome
 * de référence), puis 9600 (défaut d'un projet Nextion neuf), puis le reste. */
static const uint32_t s_baud_candidates[] = {
    115200, 9600, 921600, 57600, 38400, 19200, 230400, 250000, 256000, 512000, 4800, 2400,
};

uint32_t nextion_scan_baud(void)
{
    if (!nx_bus_acquire(5000)) {
        ESP_LOGW(TAG, "balayage annulé : bus écran occupé");
        return 0;
    }

    rx_pause();
    for (size_t i = 0; i < sizeof(s_baud_candidates) / sizeof(s_baud_candidates[0]); i++) {
        if (try_baud(s_baud_candidates[i], 300)) {
            s_baud_current = s_baud_candidates[i];
            ESP_LOGI(TAG, "écran détecté à %u bauds", (unsigned)s_baud_current);
            rx_resume();
            nx_bus_release();
            return s_baud_current;
        }
    }
    /* Aucun candidat : ne pas laisser l'UART sur un débit exotique. */
    uart_set_baudrate(NX_UART_NUM, NX_BAUD_BOOT);
    s_baud_current = 0;
    ESP_LOGW(TAG, "aucun débit ne répond (%d candidats testés)",
             (int)(sizeof(s_baud_candidates) / sizeof(s_baud_candidates[0])));
    rx_resume();
    nx_bus_release();
    return 0;
}

uint32_t nextion_current_baud(void) { return s_baud_current; }

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

    /* On NE négocie PAS `baud=` : la config ESPHome de référence
     * (Blackymas/NSPanel_HA_Blueprint) qui fonctionne sur ce matériel reste au
     * débit du .tft, et un `baud=921600` non reconnu suffit à rendre l'analyseur
     * de l'écran d'origine muet pour toute la session (constaté).
     * On DÉCOUVRE le débit à la place — il change à chaque upload de .tft. */
    if (!nextion_scan_baud()) {
        ESP_LOGW(TAG, "aucune réponse de l'écran (TX=%d RX=%d). Causes possibles, "
                      "dans l'ordre : reparse mode non quitté, écran encore en "
                      "démarrage, câblage.", NX_TX_PIN, NX_RX_PIN);
    }

    s_bus_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_bus_lock) return ESP_ERR_NO_MEM;

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
    rx_pause();
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

/* Point de contrôle grossier, lisible via /status sans moniteur série — utile
 * pour localiser un blocage dans une séquence qui n'a normalement aucune
 * raison de dépasser quelques secondes. */
static char s_last_step[48] = "-";
static void set_step(const char *s) { strncpy(s_last_step, s, sizeof(s_last_step) - 1); }
const char *nextion_last_step(void) { return s_last_step; }

/* Le verrou du bus est pris ici et relâché par nx_tft_upload_end(), pas à
 * chaque bloc : c'est TOUT le transfert qui doit être exclusif. Une seule
 * écriture d'UI intercalée décale le .tft reçu sans qu'aucun accusé ne
 * manque — l'écran boote alors sur « System data ERROR! ». Asymétrique, donc :
 * si begin échoue il relâche lui-même, et l'appelant n'appelle end() que si
 * begin a réussi (voir ota/tft_ota.c). */
bool nx_tft_upload_begin(uint32_t size_bytes, uint32_t baud, uint32_t timeout_ms)
{
    set_step("upload_begin: attente du bus écran");
    if (!nx_bus_acquire(10000)) {
        ESP_LOGE(TAG, "upload TFT: bus écran occupé, transfert refusé");
        set_step("upload_begin: bus occupé");
        return false;
    }
    set_step("upload_begin: suspend rx_task");
    rx_pause();

    /* Le débit courant de l'écran est celui de son .tft, pas forcément le nôtre :
     * le (re)découvrir avant tout, sinon whmi-wri part dans le vide. */
    set_step("upload_begin: scan baud");
    uint32_t detected = s_baud_current ? s_baud_current : nextion_scan_baud();
    rx_pause();   /* nextion_scan_baud() a pu relâcher la pause */

    /* Un écran affichant « System data ERROR! » n'a PLUS de .tft valide : il ne
     * répond donc à aucun `get`, et le balayage échoue. C'est précisément l'état
     * dont un upload doit pouvoir le sortir — on ne renonce jamais ici, on tente
     * whmi-wri sur chaque débit candidat jusqu'à obtenir l'accusé. */
    const uint32_t *cands = s_baud_candidates;
    size_t ncands = sizeof(s_baud_candidates) / sizeof(s_baud_candidates[0]);
    uint32_t only[1];
    if (detected) { only[0] = detected; cands = only; ncands = 1; }
    else ESP_LOGW(TAG, "upload TFT: écran muet (.tft invalide ?) — whmi-wri sur %u débits",
                  (unsigned)ncands);

    bool ok = false;
    uint32_t used = detected ? detected : NX_BAUD_BOOT;
    for (size_t i = 0; i < ncands && !ok; i++) {
        uint32_t cur = cands[i];
        set_step("upload_begin: exit reparse");
        uart_set_baudrate(NX_UART_NUM, cur);
        /* Écran d'origine Sonoff : son programme réaffirme le reparse mode en
         * continu — le quitter au boot de l'ESP32 ne suffit pas. Sans effet sur
         * un écran déjà en mode passif. */
        exit_reparse_mode();

        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            /* whmi-wri s'envoie au débit COURANT de l'écran ; le basculement vers
             * le débit d'upload n'a lieu qu'APRÈS, des deux côtés. L'envoyer déjà
             * au nouveau débit (bug corrigé ici) ne marchait que par coïncidence,
             * tant que les deux valaient 115200. */
            set_step("upload_begin: send whmi-wri");
            uart_set_baudrate(NX_UART_NUM, cur);
            uart_flush_input(NX_UART_NUM);
            nx_cmd("whmi-wri=%u,%u,0", (unsigned)size_bytes, (unsigned)baud);
            uart_wait_tx_done(NX_UART_NUM, pdMS_TO_TICKS(500));  /* avant de changer de débit */

            set_step("upload_begin: wait_ack");
            uart_set_baudrate(NX_UART_NUM, baud);
            ok = wait_ack_byte(timeout_ms);
        }
        if (ok) used = cur;
    }

    set_step(ok ? "upload_begin: ack reçu" : "upload_begin: timeout ack");
    if (ok) {
        s_baud_current = used;
        ESP_LOGI(TAG, "upload TFT: accusé obtenu (écran à %u bauds)", (unsigned)used);
    } else {
        ESP_LOGE(TAG, "upload TFT: pas d'accusé initial (whmi-wri), tous débits tentés");
        uart_set_baudrate(NX_UART_NUM, NX_BAUD_BOOT);   /* ne pas laisser l'UART désaccordé */
        nx_bus_release();
    }
    return ok;
}

size_t nx_raw_exchange(const char *cmd, uint32_t baud, uint8_t *rx, size_t cap,
                       uint32_t window_ms)
{
    if (!nx_bus_acquire(5000)) return 0;
    rx_pause();
    if (baud) uart_set_baudrate(NX_UART_NUM, baud);
    uart_flush_input(NX_UART_NUM);
    if (cmd && cmd[0]) nx_cmd("%s", cmd);
    size_t n = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)window_ms * 1000;
    while (esp_timer_get_time() < deadline && n < cap) {
        uint8_t b;
        if (uart_read_bytes(NX_UART_NUM, &b, 1, pdMS_TO_TICKS(20)) == 1) rx[n++] = b;
    }
    if (baud) {
        /* Revenir au débit de croisière courant plutôt que de rester sur celui
         * de l'essai — try_baud est statique plus haut, on retente les deux. */
        if (!try_baud(NX_BAUD_FAST, 200)) try_baud(NX_BAUD_BOOT, 200);
    }
    rx_resume();
    nx_bus_release();
    return n;
}

bool nx_tft_upload_chunk(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    set_step("upload_chunk: write");
    uart_write_bytes(NX_UART_NUM, data, len);
    set_step("upload_chunk: wait_ack");
    bool ok = wait_ack_byte(timeout_ms);
    set_step(ok ? "upload_chunk: ack reçu" : "upload_chunk: timeout ack");
    return ok;
}

void nx_tft_upload_end(void)
{
    /* L'écran redémarre seul après le dernier bloc accusé. Le .tft fraîchement
     * chargé impose SON débit, qui n'a aucune raison d'être celui d'avant :
     * laisser le temps au STM32 de redémarrer, puis re-balayer. */
    set_step("upload_end: attente redémarrage écran");
    vTaskDelay(pdMS_TO_TICKS(3000));
    nextion_force_reprobe();
    set_step("upload_end: scan baud");
    s_baud_current = 0;          /* forcer un vrai balayage, pas le débit d'avant */
    nextion_scan_baud();
    set_step("upload_end: probe");
    nextion_probe();   /* pause/reprise de la tâche RX incluse */
    set_step("upload_end: terminé");
    nx_bus_release();  /* pris par nx_tft_upload_begin() */
}

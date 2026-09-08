#include "nspanel_io.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "nspanel_io";

/* --- Broches (E32-MSW-NX, confirmées communauté Tasmota) --- */
#define GPIO_SCREEN_PWR   4    /* actif BAS : 0 = écran alimenté */
#define GPIO_RELAY1       22
#define GPIO_RELAY2       19
#define GPIO_BUTTON1      14   /* relais 1 */
#define GPIO_BUTTON2      27   /* relais 2 */
#define GPIO_BUZZER       21
#define GPIO_NTC          38   /* ADC1_CHANNEL_2 */

#define NX_NVS_NS         "nspanel"
#define LONGPRESS_MS       3000
#define DEBOUNCE_MS         30

static const int s_relay_gpio[NSPANEL_RELAY_COUNT]  = { GPIO_RELAY1, GPIO_RELAY2 };
static const int s_button_gpio[NSPANEL_RELAY_COUNT] = { GPIO_BUTTON1, GPIO_BUTTON2 };

static bool s_relay_state[NSPANEL_RELAY_COUNT];
static bool s_button_detached[NSPANEL_RELAY_COUNT];

static nspanel_relay_cb_t     s_relay_cb;
static nspanel_longpress_cb_t s_longpress_cb;
static nspanel_button_cb_t    s_button_cb;
static nspanel_ntc_cb_t       s_ntc_cb;

static float s_ntc_last_c = NAN;
static bool  s_boot_bridge;

/* ---- Relais ---- */

static void relay_apply(int idx, bool on)
{
    s_relay_state[idx] = on;
    gpio_set_level(s_relay_gpio[idx], on ? 1 : 0);
}

static void relay_persist(int idx, bool on)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "rly%d", idx);
    nvs_set_u8(h, key, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool nspanel_relay_get(int idx)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return false;
    return s_relay_state[idx];
}

void nspanel_relay_set(int idx, bool on)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return;
    relay_apply(idx, on);
    relay_persist(idx, on);
}

/* ---- Détachement bouton/relais ---- */

static void detach_persist(int idx, bool detached)
{
    nvs_handle_t h;
    if (nvs_open(NX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "dtch%d", idx);
    nvs_set_u8(h, key, detached ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool nspanel_io_button_detached(int idx)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return false;
    return s_button_detached[idx];
}

void nspanel_io_set_button_detached(int idx, bool detached)
{
    if (idx < 0 || idx >= NSPANEL_RELAY_COUNT) return;
    s_button_detached[idx] = detached;
    detach_persist(idx, detached);
}

/* ---- Boutons : ISR légère -> file -> tâche (débounce + détection appui long) ---- */

typedef struct { int gpio; int64_t t_us; int level; } io_evt_t;
static QueueHandle_t s_io_q;

static void IRAM_ATTR button_isr(void *arg)
{
    io_evt_t e = { .gpio = (int)(intptr_t)arg, .t_us = esp_timer_get_time(),
                   .level = gpio_get_level((int)(intptr_t)arg) };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_io_q, &e, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static int idx_for_gpio(int gpio)
{
    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++)
        if (s_button_gpio[i] == gpio) return i;
    return -1;
}

static void io_task(void *arg)
{
    (void)arg;
    int64_t press_start[NSPANEL_RELAY_COUNT] = { 0 };
    int64_t last_evt[NSPANEL_RELAY_COUNT] = { 0 };

    for (;;) {
        io_evt_t e;
        if (xQueueReceive(s_io_q, &e, portMAX_DELAY) != pdTRUE) continue;
        int idx = idx_for_gpio(e.gpio);
        if (idx < 0) continue;

        /* Anti-rebond : ignore tout événement trop proche du précédent. */
        if (e.t_us - last_evt[idx] < (int64_t)DEBOUNCE_MS * 1000) continue;
        last_evt[idx] = e.t_us;

        bool pressed = (e.level == 0);   /* actif bas (pull-up interne) */
        if (pressed) {
            press_start[idx] = e.t_us;
            continue;   /* action décidée au relâchement (durée connue) */
        }

        if (press_start[idx] == 0) continue;   /* relâchement sans appui vu (boot) */
        int64_t held_ms = (e.t_us - press_start[idx]) / 1000;
        press_start[idx] = 0;

        bool long_press = held_ms >= LONGPRESS_MS;

        if (idx == 0 && long_press) {
            ESP_LOGI(TAG, "bouton 1 : appui long (%lld ms) -> re-sondage écran",
                     (long long)held_ms);
            if (s_longpress_cb) s_longpress_cb();
        }

        /* Remonté systématiquement, même détaché ou en appui long : c'est ce
         * qui alimente l'entité HA `event.*` — le local-first ne concerne que
         * le GPIO relais, pas la visibilité de l'appui côté HA. */
        if (s_button_cb) s_button_cb(idx, long_press);

        if (long_press || s_button_detached[idx]) continue;

        bool new_state = !s_relay_state[idx];
        relay_apply(idx, new_state);
        relay_persist(idx, new_state);
        ESP_LOGI(TAG, "bouton %d -> relais %d = %s", idx + 1, idx + 1, new_state ? "ON" : "OFF");
        if (s_relay_cb) s_relay_cb(idx, new_state);
    }
}

void nspanel_io_set_relay_cb(nspanel_relay_cb_t cb)         { s_relay_cb = cb; }
void nspanel_io_set_longpress_cb(nspanel_longpress_cb_t cb) { s_longpress_cb = cb; }
void nspanel_io_set_button_cb(nspanel_button_cb_t cb)       { s_button_cb = cb; }
void nspanel_io_set_ntc_cb(nspanel_ntc_cb_t cb)             { s_ntc_cb = cb; }

/* ---- Buzzer (LEDC) ---- */

#define BUZZER_LEDC_TIMER   LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_FREQ_HZ      2700

static void buzzer_off_cb(void *arg)
{
    (void)arg;
    ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
}

void nspanel_buzzer_beep(uint32_t ms)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 128); /* ~50% */
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t cfg = { .callback = buzzer_off_cb, .name = "buzz_off" };
        esp_timer_create(&cfg, &t);
    } else {
        esp_timer_stop(t);
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

/* ---- NTC (ADC1_CHANNEL_2 / GPIO38) ---- */

static adc_oneshot_unit_handle_t s_adc;

/* Pont diviseur + Steinhart-Hart simplifié (bêta). Coefficients à affiner sur
 * banc : ceux-ci supposent un NTC 10k/B3950 typique et un pont 10k vers 3V3,
 * cohérent avec les schémas communautaires du NSPanel. */
#define NTC_BETA        3950.0f
#define NTC_T0_K        298.15f  /* 25 °C */
#define NTC_R0_OHM      10000.0f
#define NTC_RPULLUP_OHM 10000.0f
#define NTC_VREF_MV     3300.0f

static float adc_mv_to_celsius(int mv)
{
    if (mv <= 0 || mv >= (int)NTC_VREF_MV) return NAN;
    float r_ntc = NTC_RPULLUP_OHM * (float)mv / (NTC_VREF_MV - (float)mv);
    float inv_t = 1.0f / NTC_T0_K + logf(r_ntc / NTC_R0_OHM) / NTC_BETA;
    return (1.0f / inv_t) - 273.15f;
}

static void ntc_timer_cb(void *arg)
{
    (void)arg;
    if (!s_adc) return;
    int raw = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_2, &raw) != ESP_OK) return;
    /* Lecture brute -> mV approximatif (12 bits, pleine échelle ~3300 mV avec
     * atténuation 12 dB) ; suffisant pour un indicatif, pas une mesure de labo. */
    int mv = (raw * (int)NTC_VREF_MV) / 4095;
    float c = adc_mv_to_celsius(mv);
    s_ntc_last_c = c;
    if (s_ntc_cb) s_ntc_cb(c);
}

float nspanel_ntc_last_c(void) { return s_ntc_last_c; }
bool  nspanel_io_boot_bridge_requested(void) { return s_boot_bridge; }
bool  nspanel_io_screen_powered(void) { return gpio_get_level(GPIO_SCREEN_PWR) == 0; }

/* ---- Init ---- */

esp_err_t nspanel_io_init(void)
{
    /* Alimentation écran EN PREMIER : sans elle, l'écran reste noir et muet
     * quoi que fasse nextion_init() ensuite.
     *
     * ATTENTION, la ligne est ACTIVE À L'ÉTAT BAS : niveau 0 = écran alimenté.
     * (ESPHome la déclare `pin: {number: 4, inverted: true}` +
     * `restore_mode: ALWAYS_ON`, ce qui revient à piloter le GPIO à 0.)
     * Piloter ce GPIO à 1 laisse l'écran hors tension — il paraît alors
     * simplement absent, sans aucun message d'erreur pour le signaler. */
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << GPIO_SCREEN_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    /* Toujours forcer un vrai cycle d'alimentation de l'écran, même si GPIO4
     * était déjà à 0 avant ce boot (cas d'un esp_restart() après OTA) : le
     * Nextion a son propre MCU (STM32G0), qui n'est PAS réinitialisé par un
     * redémarrage logiciel de l'ESP32. Sans coupure réelle, son état interne
     * (dont un plantage éventuel) survit à l'esp_restart() et l'écran reste
     * noir/muet tant qu'on ne débranche pas manuellement le secteur. */
    gpio_set_level(GPIO_SCREEN_PWR, 1);              /* écran OFF */
    vTaskDelay(pdMS_TO_TICKS(300));                  /* décharge avant rallumage */
    gpio_set_level(GPIO_SCREEN_PWR, 0);              /* écran ON */

    /* Relais. */
    gpio_config_t rly = {
        .pin_bit_mask = (1ULL << GPIO_RELAY1) | (1ULL << GPIO_RELAY2),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rly);
    nvs_handle_t h;
    bool have_nvs = (nvs_open(NX_NVS_NS, NVS_READONLY, &h) == ESP_OK);
    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++) {
        uint8_t v = 0;
        if (have_nvs) {
            char key[16];
            snprintf(key, sizeof(key), "rly%d", i);
            nvs_get_u8(h, key, &v);
        }
        relay_apply(i, v != 0);
    }
    for (int i = 0; i < NSPANEL_RELAY_COUNT; i++) {
        uint8_t v = 0;
        if (have_nvs) {
            char key[16];
            snprintf(key, sizeof(key), "dtch%d", i);
            nvs_get_u8(h, key, &v);
        }
        s_button_detached[i] = (v != 0);
    }
    if (have_nvs) nvs_close(h);

    /* Boutons : entrée + pull-up interne, interruption sur les deux fronts. */
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << GPIO_BUTTON1) | (1ULL << GPIO_BUTTON2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn);

    /* Déclencheur physique du pont série : bouton 2 tenu ~700 ms au boot.
     * Sondé AVANT l'ISR pour ne pas se confondre avec l'usage normal du bouton. */
    if (gpio_get_level(GPIO_BUTTON2) == 0) {
        bool held = true;
        for (int i = 0; i < 14; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(GPIO_BUTTON2) != 0) { held = false; break; }
        }
        s_boot_bridge = held;
        if (held) ESP_LOGW(TAG, "bouton 2 tenu au boot -> pont série demandé");
    }

    s_io_q = xQueueCreate(16, sizeof(io_evt_t));
    if (!s_io_q) return ESP_ERR_NO_MEM;

    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON1, button_isr, (void *)(intptr_t)GPIO_BUTTON1);
    gpio_isr_handler_add(GPIO_BUTTON2, button_isr, (void *)(intptr_t)GPIO_BUTTON2);

    if (xTaskCreate(io_task, "nspanel_io", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(nspanel_io) échoué");
        return ESP_ERR_NO_MEM;
    }

    /* Buzzer (LEDC, silencieux par défaut). */
    ledc_timer_config_t ltimer = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ltimer);
    ledc_channel_config_t lchan = {
        .gpio_num = GPIO_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
    };
    ledc_channel_config(&lchan);

    /* NTC. */
    adc_oneshot_unit_init_cfg_t adc_unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&adc_unit_cfg, &s_adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc, ADC_CHANNEL_2, &chan_cfg);
        static esp_timer_handle_t ntc_timer;
        const esp_timer_create_args_t tcfg = { .callback = ntc_timer_cb, .name = "ntc" };
        if (esp_timer_create(&tcfg, &ntc_timer) == ESP_OK) {
            ntc_timer_cb(NULL);   /* première lecture immédiate */
            esp_timer_start_periodic(ntc_timer, 30ULL * 1000 * 1000);
        }
    } else {
        ESP_LOGW(TAG, "ADC NTC indisponible");
    }

    return ESP_OK;
}

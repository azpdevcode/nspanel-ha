#include "app_diag.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "diag";

void app_diag_log_mem(const char *phase)
{
    /* min = plus bas niveau jamais atteint depuis le boot : c'est la marge réelle.
     * S'il approche 0, un xTaskCreate finira par échouer. big = plus grand bloc
     * contigu (fragmentation) — décisif ici, seul tas disponible (pas de PSRAM). */
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t big      = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "[%s] libre=%u min=%u bloc=%u (octets)",
             phase ? phase : "?",
             (unsigned)free_now, (unsigned)free_min, (unsigned)big);
}

static void diag_timer_cb(void *arg)
{
    (void)arg;
    app_diag_log_mem("périodique");
}

void app_diag_start(void)
{
    static esp_timer_handle_t s_timer;
    if (s_timer) return;   /* déjà démarré */
    const esp_timer_create_args_t cfg = {
        .callback = diag_timer_cb,
        .name = "diag_mem",
    };
    if (esp_timer_create(&cfg, &s_timer) == ESP_OK)
        esp_timer_start_periodic(s_timer, 30ULL * 1000 * 1000);   /* 30 s */
    else
        ESP_LOGW(TAG, "moniteur mémoire indisponible (esp_timer_create)");
}

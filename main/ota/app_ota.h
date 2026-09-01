/*
 * Mise à jour du firmware ESP32 depuis une URL HTTP.
 *
 * Sans ça, chaque correction firmware impose une séance USB-TTL avec IO0 à la
 * masse (le NSPanel n'a pas d'auto-reset, cf. README « Premier flash »), et
 * l'adaptateur ne peut pas cohabiter avec l'alimentation secteur.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_OTA_IDLE,      /* jamais lancé depuis le boot */
    APP_OTA_RUNNING,
    APP_OTA_OK,        /* écrit et validé — redémarrage imminent */
    APP_OTA_FAILED,
} app_ota_state_t;

/**
 * @brief Télécharge le .bin à `url`, l'écrit dans la partition OTA inactive,
 *        et redémarre sur elle.
 *
 * Bloquant (quelques dizaines de secondes). À appeler depuis une tâche dédiée
 * avec au moins 8 Ko de pile. Ne rend pas la main en cas de succès : l'ESP32
 * redémarre. En cas d'échec, la partition courante reste intacte et bootable —
 * rien n'est basculé tant que l'image n'est pas complète et valide.
 *
 * Le rollback bootloader est actif (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) :
 * si la nouvelle image ne va pas jusqu'à esp_ota_mark_app_valid_cancel_rollback()
 * dans app_main(), le bootloader revient tout seul à l'ancienne au reset
 * suivant. Un firmware qui ne démarre pas ne brique donc pas le panneau.
 */
esp_err_t app_ota_run(const char *url);

/** @brief État de la dernière (ou de la) mise à jour applicative. */
app_ota_state_t app_ota_get_state(void);

/** @brief Détail lisible de l'état courant (jamais NULL). */
const char *app_ota_get_status(void);

#ifdef __cplusplus
}
#endif

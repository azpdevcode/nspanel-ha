/*
 * Téléversement du .tft de l'écran depuis une URL HTTP (typiquement /local/
 * sur Home Assistant), via le protocole d'upload Nextion (whmi-wri).
 * Alternative au pont série pour mettre à jour l'écran une fois au mur.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Télécharge le .tft à `url` et le transfère à l'écran par blocs de 4 Ko.
 *
 * Bloquant (plusieurs dizaines de secondes selon la taille du .tft et le débit
 * réseau). Suspend les événements tactiles pendant le transfert (voir
 * nx_tft_upload_begin/nextion_probe). À appeler depuis une tâche dédiée, pas
 * depuis ui_worker (pour ne pas bloquer le reste de l'UI aussi longtemps).
 */
esp_err_t tft_ota_run(const char *url);

typedef enum {
    TFT_OTA_IDLE,      /* jamais lancé depuis le boot */
    TFT_OTA_RUNNING,
    TFT_OTA_OK,
    TFT_OTA_FAILED,
} tft_ota_state_t;

/** @brief État du dernier (ou du) transfert TFT-OTA. */
tft_ota_state_t tft_ota_get_state(void);

/** @brief Détail lisible de l'état courant (jamais NULL, ex. "connexion échouée: ..."). */
const char *tft_ota_get_status(void);

#ifdef __cplusplus
}
#endif

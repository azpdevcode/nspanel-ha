/*
 * Routes de développement greffées sur le portail web (config_portal) :
 * POST /bridge  -> bascule l'ESP32 en pont série transparent USB<->écran, pour
 *                  téléverser un .tft depuis Nextion Editor sans démonter le
 *                  panneau (alternative au bouton physique au boot).
 * POST /tft     -> déclenche un TFT-OTA HTTP (voir ota/tft_ota.h), corps
 *                  form-urlencoded "url=...".
 *
 * Séparé de config_portal.c pour garder ce dernier proche de sa version
 * d'origine (compte HA uniquement) ; ce fichier est spécifique au fork.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** À appeler après config_portal_start(). Ajoute les routes sur son serveur HTTP. */
esp_err_t dev_portal_register(void);

#ifdef __cplusplus
}
#endif

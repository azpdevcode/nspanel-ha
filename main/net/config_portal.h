#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Démarre le portail web local de configuration du compte Home Assistant.
 *
 * Lance un serveur HTTP minimal (esp_http_server, port 80) sur l'interface
 * station. Depuis un appareil du réseau local, ouvrir http://<ip-panneau>/ pour
 * coller l'URL HA et le token longue durée (évite la saisie tactile du token).
 * L'enregistrement déclenche un redémarrage du panneau.
 *
 * À appeler après wifi_start() (la pile réseau doit être initialisée). N'est
 * joignable qu'une fois le WiFi connecté (le panneau a une IP).
 *
 * @return ESP_OK si le serveur a démarré.
 */
esp_err_t config_portal_start(void);

/** @brief Handle du serveur HTTP interne, pour y greffer d'autres routes
 *  (voir net/dev_portal.h). NULL si config_portal_start() n'a pas été appelé. */
httpd_handle_t config_portal_handle(void);

#ifdef __cplusplus
}
#endif

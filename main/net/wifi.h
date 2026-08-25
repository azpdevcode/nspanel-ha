#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"   /* wifi_ap_record_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise la pile réseau et lance la connexion WiFi (mode station).
 *
 * La connexion est asynchrone ; utiliser wifi_wait_connected() pour bloquer.
 *
 * SSID / mot de passe proviennent de app_config (NVS, repli Kconfig). Si aucun
 * SSID n'est configuré, la station démarre mais ne tente pas de connexion :
 * l'utilisateur configure le WiFi depuis le panneau via wifi_apply().
 *
 * @return ESP_OK si l'initialisation a démarré correctement.
 */
esp_err_t wifi_start(void);

/**
 * @brief Bloque jusqu'à l'obtention d'une IP (ou le timeout).
 *
 * @param timeout_ms  délai max ; 0 = attente illimitée.
 * @return true si connecté (IP obtenue), false si timeout.
 */
bool wifi_wait_connected(uint32_t timeout_ms);

/** @brief Indique si une IP est actuellement obtenue. */
bool wifi_is_connected(void);

/** @brief Indique si la dernière tentative a échoué (max retries atteint). */
bool wifi_failed(void);

/**
 * @brief Scan bloquant des réseaux à portée (~1-2 s). À appeler hors thread LVGL.
 *
 * Remplit `out` avec au plus `max` enregistrements (triés par RSSI décroissant
 * par la pile). Ne dédoublonne pas les SSID (à faire par l'appelant).
 *
 * @param out    tableau fourni par l'appelant.
 * @param max    capacité de `out`.
 * @param found  [out] nombre d'AP réellement écrits.
 * @return ESP_OK si le scan a abouti.
 */
esp_err_t wifi_scan(wifi_ap_record_t *out, int max, int *found);

/**
 * @brief Applique de nouveaux identifiants WiFi à chaud (persiste + reconnecte).
 *
 * Enregistre (app_config), réinitialise l'état, se déconnecte puis se reconnecte
 * au nouveau réseau. Sonder ensuite wifi_is_connected() / wifi_failed().
 *
 * @return ESP_OK si la (re)connexion a été lancée.
 */
esp_err_t wifi_apply(const char *ssid, const char *pass);

/**
 * @brief Retourne l'IP courante de la station (format "x.x.x.x").
 *
 * @param out  tampon fourni par l'appelant.
 * @param n    taille du tampon (≥16 pour une IPv4 complète).
 * @return true si une IP est disponible, false sinon.
 */
bool wifi_get_ip(char *out, size_t n);

#ifdef __cplusplus
}
#endif

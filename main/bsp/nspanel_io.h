/*
 * E/S locales du NSPanel : alimentation écran, 2 relais + 2 boutons muraux,
 * buzzer, sonde de température NTC. Comportement "local d'abord" pour les
 * relais : un appui bascule le relais immédiatement (fonctionne WiFi coupé),
 * puis notifie l'appelant (ui_worker) pour miroir vers HA. Ce module ne connaît
 * rien de HA ni de l'écran — couplage nul, par conception.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NSPANEL_RELAY_COUNT 2

/** Appelé (hors ISR, contexte tâche) après un changement d'état LOCAL d'un
 *  relais (bouton physique). idx : 0 ou 1. */
typedef void (*nspanel_relay_cb_t)(int idx, bool on);

/** Appelé (hors ISR) après un appui long (≥3 s) sur le bouton 1 : déclenche un
 *  re-sondage complet de l'écran depuis l'appelant (voir nextion_force_reprobe). */
typedef void (*nspanel_longpress_cb_t)(void);

/** Appelé toutes les ~30 s avec la température NTC en °C (NAN si lecture invalide). */
typedef void (*nspanel_ntc_cb_t)(float celsius);

/**
 * @brief Initialise GPIO4 (alimentation écran, actif BAS — mis à 0 immédiatement,
 * sans cela l'écran reste noir), les relais (restaurés depuis NVS), les boutons
 * (interruption + anti-rebond) et l'ADC de la sonde NTC.
 *
 * À appeler avant nextion_init().
 */
esp_err_t nspanel_io_init(void);

/** @brief true si GPIO4 est actuellement à l'état bas (écran alimenté).
 *  Lecture directe du GPIO, pas juste ce qu'on a demandé à nspanel_io_init(). */
bool nspanel_io_screen_powered(void);

/** true si le bouton 2 était maintenu enfoncé pendant les ~700 ms qui ont suivi
 *  nspanel_io_init() — déclencheur physique du pont série (voir nextion_bridge_run). */
bool nspanel_io_boot_bridge_requested(void);

void nspanel_io_set_relay_cb(nspanel_relay_cb_t cb);
void nspanel_io_set_longpress_cb(nspanel_longpress_cb_t cb);
void nspanel_io_set_ntc_cb(nspanel_ntc_cb_t cb);

bool nspanel_relay_get(int idx);
/** Bascule programmatique (ex. écho d'un changement venu de HA) : ne ré-émet
 *  PAS le callback relais (évite une boucle avec ui_worker). */
void nspanel_relay_set(int idx, bool on);

/** Bip court (LEDC sur GPIO21), accusé de réception tactile. */
void nspanel_buzzer_beep(uint32_t ms);

/** Dernière lecture NTC en cache (°C), NAN si aucune lecture valide encore. */
float nspanel_ntc_last_c(void);

#ifdef __cplusplus
}
#endif

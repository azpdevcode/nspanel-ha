#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Client MQTT — publie les relais/boutons/sonde du panneau vers Home Assistant
 * par MQTT discovery (homeassistant/<composant>/<node>/<objet>/config), et
 * reçoit en retour les commandes HA sur les relais et le détachement bouton.
 *
 * Contrairement à ha_ws (lecture, WebSocket) et ha_client (écriture ponctuelle,
 * REST), ce module PUBLIE l'état du panneau : jusqu'ici rien ne rendait les
 * relais/boutons visibles dans HA (voir design/FORK-PLAN.md). N'importe rien
 * de l'UI ni de l'écran — mêmes principes de couplage que nspanel_io.
 *
 * no-op complet (ha_mqtt_start renvoie ESP_OK sans rien faire) si aucun broker
 * n'est configuré (app_config_mqtt_set() == false) : le panneau doit rester
 * utilisable sans MQTT.
 */

esp_err_t ha_mqtt_start(void);

/** true si la session MQTT est connectée au broker. */
bool ha_mqtt_is_connected(void);

/** Publie l'état retenu d'un relais (0/1) — ON/OFF sur relay/<idx+1>/state. */
void ha_mqtt_publish_relay(int idx, bool on);

/** Publie un appui bouton (non retenu) — {"event_type":"press"|"long_press"}. */
void ha_mqtt_publish_button(int idx, bool long_press);

/** Publie l'état détaché d'un bouton (retenu) — ON/OFF sur detach/<idx+1>/state. */
void ha_mqtt_publish_detached(int idx, bool detached);

/** Publie la température NTC (déjà corrigée de l'offset), retenue. NAN ignoré. */
void ha_mqtt_publish_temperature(float celsius);

/** Publie RSSI WiFi + uptime (entités diagnostic), retenues. À appeler
 *  périodiquement (~60 s) depuis la boucle de battement de cœur d'app_main. */
void ha_mqtt_publish_diag(void);

typedef struct {
    bool     connected;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t messages_published;
    uint32_t commands_received;
} ha_mqtt_stats_t;
void ha_mqtt_get_stats(ha_mqtt_stats_t *out);

#ifdef __cplusplus
}
#endif

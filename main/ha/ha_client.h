#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Couche client Home Assistant (API REST). Extrait et allégé de ha-panel :
 * plus de médias, pochettes, caméras, agendas, timers, notifications, alarme,
 * logbook — voir design/FORK-PLAN.md, section « Stripping de ha_client.c ».
 *
 * Fonctions bloquantes (réseau) : à appeler depuis ha_worker/rooms, jamais
 * depuis la tâche ui (elle ne doit jamais attendre sur le réseau).
 */

/** État riche d'une lumière : on/off + variation (dimmable) + luminosité (0..255). */
typedef struct { bool on; bool dimmable; int brightness; } ha_light_state_t;

/* Parse l'état on/off depuis un objet état HA (forme /api/states/<e>, identique
 * au new_state d'un event WS). Réutilisé par le chemin push (ha_ws). */
esp_err_t ha_parse_light_state(cJSON *root, bool *is_on);
esp_err_t ha_parse_light(cJSON *root, ha_light_state_t *out);
esp_err_t ha_get_light(const char *entity_id, ha_light_state_t *out);

/** Règle la luminosité (1..100 %) via light.turn_on. Réservé aux entités
 *  dimmable du domaine light. : sans effet correct sur un switch.* labellisé. */
esp_err_t ha_light_set_brightness(const char *entity_id, int pct);

/** Bascule (toggle) l'état d'une entité — homeassistant/toggle, générique :
 *  valide pour light.* ET switch.* (les switch.* labellisés « Light ha panel »
 *  passent par ici). */
esp_err_t ha_light_toggle(const char *entity_id);
/** Force l'état on/off (turn_on / turn_off), même portée que ha_light_toggle. */
esp_err_t ha_light_set(const char *entity_id, bool on);

/** Active une scène (scene.turn_on). */
esp_err_t ha_scene_activate(const char *entity_id);
/** Lance un script (script.turn_on). */
esp_err_t ha_script_run(const char *entity_id);

/**
 * @brief Rend un template Jinja côté HA (POST /api/template) et renvoie le texte.
 * @param[in]  tmpl     template Jinja.
 * @param[out] out      buffer recevant le rendu (terminé par \0).
 * @param[in]  out_len  taille du buffer.
 */
esp_err_t ha_render_template(const char *tmpl, char *out, size_t out_len);

/* ---- Chauffage (climate) — posé pour la phase 3 ---- */
typedef struct {
    char  state[16];      /* hvac_mode : auto / heat / off / unavailable */
    char  action[16];     /* hvac_action : heating / idle / off */
    char  preset[24];     /* preset_mode courant */
    float current_temp;   /* °C mesurés */
    float target_temp;    /* °C consigne */
    float min_temp, max_temp, step;
    bool  ok;
} ha_climate_state_t;

esp_err_t ha_parse_climate_state(cJSON *root, ha_climate_state_t *out);
esp_err_t ha_get_climate_state(const char *entity_id, ha_climate_state_t *out);
esp_err_t ha_climate_set_temp(const char *entity_id, float temp);
esp_err_t ha_climate_set_preset(const char *entity_id, const char *preset);

/** Pression atmosphérique d'une entité weather.* (NAN si absente/échec). */
esp_err_t ha_get_weather_pressure(const char *entity_id, float *pressure);

#ifdef __cplusplus
}
#endif

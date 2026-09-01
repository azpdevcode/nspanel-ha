/*
 * Pilote de l'écran Nextion déporté (STM32G0, UART, protocole ASCII + retours
 * binaires). Remplace toute la couche LVGL du panneau P4 : il n'y a ici ni
 * framebuffer ni rendu côté ESP32, seulement des commandes texte et des
 * événements tactiles.
 *
 * Tolérance au TFT partiel (cf. plan) : chaque objet écrit par le firmware est
 * déclaré dans nx_objects.def et sondé une fois au boot (nextion_probe()).
 * Écrire sur un objet absent est un no-op silencieux — jamais un blocage.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Pages, générées depuis nx_pages.def --- */
typedef enum {
#define NX_PAGE(id, name) id,
#include "nx_pages.def"
#undef NX_PAGE
    NXP_COUNT
} nx_page_t;

/* --- Objets écrits par le firmware, générés depuis nx_objects.def --- */
typedef enum {
#define NX_OBJ(id, page, name, attr) id,
#include "nx_objects.def"
#undef NX_OBJ
    NX_OBJ_COUNT
} nx_obj_t;

/* Événement tactile brut (retour 0x65 du protocole Nextion). */
typedef struct {
    uint8_t page;
    uint8_t cid;      /* id du composant dans la page */
    uint8_t pressed;  /* 1 = appui, 0 = relâchement */
} nx_touch_t;

/**
 * @brief Initialise l'UART vers l'écran, négocie le débit, sonde les objets.
 *
 * Ordre : GPIO alimentation écran (voir nspanel_io.h) doit déjà être haut.
 * Ouvre à 115200, tente de monter à 921600 (repli silencieux sur 115200 si
 * l'écran ne répond pas au nouveau débit), puis appelle nextion_probe().
 * Seul échec fatal : l'UART ne s'ouvre pas du tout (matériel absent).
 */
esp_err_t nextion_init(void);

/**
 * @brief (Re)sonde tous les objets de nx_objects.def et met à jour le cache NVS.
 *
 * Lit d'abord la Variable globale `vSpec` (révision de la spec HMI) :
 *  - absente → écran d'origine ou HMI non conforme : tout est marqué absent,
 *    aucun sondage supplémentaire n'est tenté.
 *  - présente et déjà connue en NVS (même révision) → bitmap rechargé du cache,
 *    pas de sondage réseau.
 *  - présente et nouvelle → sondage complet (`get <nom>.<attr>` par objet),
 *    résultat mis en cache NVS sous cette révision.
 *
 * Suspend temporairement le traitement des événements tactiles pendant le
 * sondage (aller-retours synchrones sur le même UART).
 */
void nextion_probe(void);

/** @brief Force un nouveau sondage complet au prochain nextion_probe() (ignore le cache NVS). */
void nextion_force_reprobe(void);

/** @brief true si l'objet a été détecté présent dans le .tft actuel. */
bool nx_present(nx_obj_t o);

/** @brief true si la page est utilisable (son objet d'ancrage est présent). */
bool nx_page_present(nx_page_t p);

/** @brief true si la Variable globale `vSpec` a répondu au dernier sondage
 *  (sinon : écran d'origine ou .tft non conforme, cf. nextion_probe()). */
bool nextion_spec_present(void);

/** @brief Révision de `vSpec` lue au dernier sondage réussi, -1 si absente. */
int32_t nextion_spec_revision(void);

/** @brief Nombre d'objets du registre détectés présents / total au dernier sondage. */
void nextion_objects_summary(int *present, int *total);

/** @brief Dernier point de contrôle atteint dans le pilote (diagnostic à distance,
 *  ex. pendant un upload TFT-OTA figé, sans avoir besoin du moniteur série). */
const char *nextion_last_step(void);

/**
 * @brief Échange brut de diagnostic : envoie `cmd` (terminateur FF FF FF ajouté,
 * "" = n'envoie rien, écoute seulement) et collecte tout ce que l'écran émet
 * pendant `window_ms`. `baud` non nul force ce débit pour l'échange, puis le
 * débit de croisière est renégocié. Retourne le nombre d'octets reçus dans rx.
 * Exposé par POST /nx du portail de dev — diagnostic sans moniteur série.
 */
size_t nx_raw_exchange(const char *cmd, uint32_t baud, uint8_t *rx, size_t cap,
                       uint32_t window_ms);

/**
 * @brief Balaie les débits standard jusqu'à ce que l'écran réponde à `get sleep`.
 *
 * Indispensable : un .tft embarque son propre réglage de débit (un projet
 * Nextion neuf est souvent à 9600), qui change donc à chaque téléversement.
 * @return le débit retenu, ou 0 si aucun ne répond (l'UART est alors remis à 115200).
 */
uint32_t nextion_scan_baud(void);

/** @brief Débit auquel l'écran a répondu au dernier balayage (0 = muet). */
uint32_t nextion_current_baud(void);

/* --- Verrou du bus écran ---
 *
 * À prendre par toute séquence qui doit être exclusive sur l'UART écran : un
 * aller-retour get/réponse, ou un transfert TFT entier. Récursif (une tâche
 * qui le détient peut appeler nx_cmd()). Les écritures d'UI l'abandonnent au
 * bout de quelques dizaines de ms plutôt que d'attendre — un rafraîchissement
 * perdu se rejoue à la passe suivante, alors qu'un blocage de 45 s pendant un
 * upload TFT gèlerait la boucle applicative. */
bool nx_bus_acquire(uint32_t timeout_ms);
void nx_bus_release(void);

/* --- Écritures, no-op silencieux si l'objet est absent (retour false) --- */
bool nx_set_txt(nx_obj_t o, const char *s);
/** @brief Écrit l'attribut numérique déclaré pour l'objet dans nx_objects.def
 *         (`val` pour une Progress bar / un Slider, `pic` pour un Picture). */
bool nx_set_val(nx_obj_t o, int32_t v);
bool nx_set_pco(nx_obj_t o, uint16_t rgb565);
bool nx_vis(nx_obj_t o, bool on);

/** @brief Change de page. No-op (false) si la page est absente du .tft. */
bool nx_page(nx_page_t p);

/** @brief Rétroéclairage 0-100 (commande `dim=`). Toujours tentée (page-agnostique). */
void nx_dim(uint8_t pct);

/** @brief Envoie une commande brute formatée (le terminateur FF FF FF est ajouté). */
void nx_cmd(const char *fmt, ...);

/**
 * @brief Attend un événement tactile.
 * @param timeout_ms 0 = non bloquant, sinon délai max.
 * @return true si un événement a été récupéré dans *out.
 */
bool nextion_get_touch(nx_touch_t *out, uint32_t timeout_ms);

/**
 * @brief Pont série transparent USB (UART0) <-> écran (UART2), pour téléverser
 * un .tft depuis Nextion Editor comme sur un écran nu.
 *
 * Bloquant : ne retourne jamais (le panneau doit être redémarré pour en sortir).
 * À appeler depuis une tâche dédiée, jamais depuis ui_worker.
 */
void nextion_bridge_run(void) __attribute__((noreturn));

/* --- Upload TFT-OTA (protocole Nextion "whmi-wri", v1.2) ---
 * Utilisé par ota/tft_ota.c ; primitives bas niveau uniquement — la tâche RX
 * tactile est suspendue le temps du transfert, comme pour nextion_probe(). */

/** Annonce le transfert (taille + débit) et attend l'accusé 0x05 initial. */
bool nx_tft_upload_begin(uint32_t size_bytes, uint32_t baud, uint32_t timeout_ms);
/** Envoie un bloc puis attend son accusé 0x05 (renvoie false si absent/erreur). */
bool nx_tft_upload_chunk(const uint8_t *data, size_t len, uint32_t timeout_ms);
/** Termine le transfert : reprend la tâche RX, renégocie le débit normal, force un re-sondage. */
void nx_tft_upload_end(void);

#ifdef __cplusplus
}
#endif

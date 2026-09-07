/*
 * Primitives de dessin composées au-dessus de nx_cmd() — Nextion n'a pas de
 * commande d'arc, seulement des disques (`cirs`). Réservé à la page `home` :
 * les anneaux de température y sont des valeurs, ils n'ont rien à faire dans
 * une image statique (voir design/EDITOR-HOME.md).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dessine un arc épais en chapelet de disques jointifs.
 *
 * @param cx, cy       centre, en pixels de la page affichée.
 * @param r             rayon du tracé (centre de l'épaisseur).
 * @param thickness     épaisseur du trait, en pixels.
 * @param a_start, a_end angles en degrés, 0° = 3 h, sens horaire. a_end peut
 *                        dépasser 360 (ex. 135..405) pour un arc qui traverse 0°.
 * @param col565        couleur RGB565.
 *
 * Prend le verrou du bus une seule fois pour toute la rafale (récursif) :
 * sans ça, chaque `nx_cmd()` retente indépendamment et peut être abandonné en
 * silence si un autre appelant tient le bus, laissant un arc à moitié tracé.
 * No-op si le bus reste indisponible après l'attente (nx_bus_acquire()).
 */
void nx_draw_arc(int cx, int cy, int r, int thickness,
                 int a_start, int a_end, uint16_t col565);

#ifdef __cplusplus
}
#endif

# Montage de la page `home` dans Nextion Editor

Recette pas à pas pour construire la page 1 (`home`) à partir des PNG de
`design/images/out/` (générés par `design/images/gen_images.py`). Complète
`design/HMI-SPEC.md`, qui reste la source de vérité pour les noms d'objets et
l'allocation des `pic` id.

Tout est en **480×320 paysage**. Les coordonnées ci-dessous sont exactes : le
fond `01_bg_home.png` a été dessiné avec elles, et l'icône `pWx` est aplatie sur
le morceau de dégradé situé en `26,232` — la déplacer casse le rendu.

## 0. Prérequis

1. Projet Editor en **NX4832F035_011** (Discovery 3,5"), orientation **paysage**.
2. Polices `.zi` **générées en `iso-8859-1`** (jamais utf-8, cf. HMI-SPEC) :
   `f0` = 16 px, `f1` = 24 px, `f2` = 48 px.
3. Ressources Picture importées **dans l'ordre alphabétique des fichiers**,
   gardes `NN_reserved.png` comprises : l'id d'une Picture est son rang
   d'insertion. `01_bg_home.png` doit donc porter l'id **1**, `56_home_wx64_*`
   les id **56..64**, les deux ampoules **65/66**.
4. Page nommée exactement `home`, créée **en deuxième** (index 1) — l'ordre des
   pages doit suivre `main/bsp/nx_pages.def`.
5. `main/bsp/nx_objects.def` doit déjà contenir la ligne `pLight` (ajoutée avec
   ce montage) : `NX_OBJ(HOME_LIGHT_PIC, NXP_HOME, "pLight", "pic")`.

## 1. Attributs de la page

| Attribut | Valeur |
|---|---|
| `objname` | `home` |
| `sta` | `image` |
| `pic` | `1` (`01_bg_home.png`) |

Ce qui est **déjà dans le fond** et ne doit pas être recréé : l'engrenage
`mCfg` (444,12), la tuile Lumières — carte arrondie (8,52)→(232,190), icône et
libellé « Lumières » gravés en (16,60) —, la tuile Chauffage — carte arrondie
(248,52)→(472,190), icône et libellé « Chauffage » gravés en (256,60) —, et la
carte météo en dégradé (8,196)→(472,314) avec son icône et son titre « Météo ».

**Les anneaux de chauffage ne sont PAS dans le fond.** Ils portent une valeur
(température mesurée / consigne) et sont dessinés à l'exécution par le
firmware (`nx_draw_arc()`, `main/ui/nx_draw.c`, appelé depuis
`home_arcs_paint()` dans `main/ui/ui.c`) — en chapelet de disques `cirs`,
Nextion n'ayant pas de primitive d'arc. Géométrie, à ne rien poser dessus :
centre `(360,130)`, anneau extérieur (mesure) rayon 52 épaisseur 9, anneau
intérieur (consigne) rayon 34 épaisseur 7, plage 0..50 °C sur 270° à partir de
135° (identique à la jauge du panneau P4). Si la tuile Chauffage bouge dans
l'Editor, corriger `HOME_ARC_*` dans `ui.c` en conséquence.

## 2. Composants, dans l'ordre de création

Le firmware n'adresse les objets **que par leur nom** (`home.tClock.txt=…`,
`get home.pWx.pic`) : pour les onze premiers, l'ordre de création n'a aucune
importance, il ne fait que rendre la relecture plus simple.

Les **hotspots** sont l'exception, et pour une raison de protocole : un appui
remonte la trame `0x65 page cid event` (`main/bsp/nextion.c:504`), qui ne
transporte pas de nom — seulement deux entiers. `main/ui/nx_nav.def` doit donc
mapper ces `cid`. L'Editor les attribue par rang de création dans la page, d'où
la consigne : créer les onze objets pilotés d'abord, les quatre hotspots
ensuite, ce qui donne 12..15.

| # | `cid` | `objname` | Type | x | y | w | h | Police | `pco` | Alignement |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | — | `tClock` | Text | 14 | 4 | 150 | 44 | f2 | 61342 | gauche / centré vert. |
| 2 | — | `tDate` | Text | 176 | 20 | 180 | 20 | f0 | 35988 | gauche / centré |
| 3 | — | `pLight` | Picture | 24 | 100 | 64 | 64 | — | — | `pic` = 65 |
| 4 | — | `tRoom` | Text | 100 | 106 | 124 | 22 | f0 | 61342 | gauche / centré |
| 5 | — | `tLights` | Text | 100 | 132 | 124 | 22 | f0 | 35988 | gauche / centré |
| 6 | — | `tTemp` | Text | 324 | 114 | 72 | 26 | f1 | 62735 | centré / centré |
| 7 | — | `tSet` | Text | 330 | 142 | 60 | 18 | f0 | 60231 | centré / centré |
| 8 | — | `pWx` | Picture | 26 | 232 | 64 | 64 | — | — | `pic` = 56 |
| 9 | — | `tWxT` | Text | 100 | 232 | 160 | 56 | f2 | 61342 | gauche / centré |
| 10 | — | `tWxC` | Text | 100 | 290 | 250 | 20 | f0 | 61342 | gauche / centré |
| 11 | 12 | `mLight` | Hotspot | 8 | 52 | 224 | 138 | — | — | — |
| 12 | 13 | `mHeat` | Hotspot | 248 | 52 | 224 | 138 | — | — | — |
| 13 | 14 | `mWx` | Hotspot | 8 | 196 | 464 | 118 | — | — | — |
| 14 | 15 | `mCfg` | Hotspot | 424 | 0 | 56 | 48 | — | — | — |

`pco` est donné en RGB565 décimal, déjà arrondi à la grille 565 comme les
images (`COL_TXT` 61342, `COL_TXT_DIM` 35988, `COL_HEAT_SOFT` 62735,
`COL_HEAT` 60231 — ces deux dernières valeurs doivent rester identiques à
`COL_HEAT_SOFT_565`/`COL_HEAT_565` dans `main/ui/ui.c`, sinon le texte et
l'anneau qu'il annote n'ont pas la même couleur).

`tTemp`/`tSet` sont volontairement étroits : leurs coins doivent rester à
l'intérieur de l'anneau intérieur (rayon 34, bord externe ~37 avec
l'épaisseur), sinon le texte mord sur les disques dessinés par le firmware.

### Fond de chaque composant : `sta` = **Crop Image**, `picc` = **1**

Pour **tous** les Text ci-dessus, mettre `sta` à `crop image` et `picc` à `1`.
C'est ce qui fait qu'un texte se redessine sur les pixels réels du fond au lieu
d'un rectangle de couleur. Indispensable sur la carte météo (`tWxT`, `tWxC`) où
le fond est un dégradé, et maintenant aussi sur `tTemp`/`tSet` : le firmware y
dessine des disques par-dessus le fond entre deux écritures de texte, un
`sta = solid color` y laisserait un pavé de la mauvaise couleur visible tant
que l'objet n'a pas été réécrit. Sur le noir plat (`tClock`, `tDate`…) ça ne se
voit pas, mais autant garder la même règle partout — sinon un déplacement de
2 px trahit le pavé.

Les Picture (`pLight`, `pWx`) n'ont pas de `sta` : l'aplatissement est déjà dans
le PNG.

### Valeurs initiales (`txt`)

`tWxT`, `tWxC` : mettre `--` (writées une seule fois par le firmware au premier
affichage, `home_page_placeholders_once()` dans `main/ui/ui.c`, en attendant la
météo — phase 3). `tTemp`, `tSet` : laisser vide, le firmware les écrit dans la
seconde qui suit l'affichage de `home` (valeur réelle ou `--` selon la
disponibilité de l'entité climate). `tClock`/`tDate`/`tRoom`/`tLights` : laisser
vide, idem.

### Événements tactiles

Aucun code Nextion sur les hotspots : cocher **Send Component ID** sur *Touch
Press Event* uniquement (pas Release). Le firmware décide de la navigation à
partir de l'id reçu, via `main/ui/nx_nav.def`. Ne pas ajouter de `page N` dans
l'Editor — la page cible passerait devant un `.tft` partiel et court-circuiterait
`nx_page_present()`.

## 3. Images utilisées par cette page

| `pic` | Fichier | Objet | Remarque |
|---|---|---|---|
| 1 | `01_bg_home.png` | fond de page | 480×320, tuiles + carte météo, sans les anneaux |
| 56..64 | `56_home_wx64_*.png` | `pWx` | ordre `sunny, clear-night, partlycloudy, cloudy, rainy, snowy, lightning, windy, fog` — le firmware écrit `56 + index` |
| 65 / 66 | `65_home_light64_off.png` / `66_..._on.png` | `pLight` | aplaties sur `COL_CARD` (fond de la tuile Lumières) |

`pWx` est le seul cas lié à sa position : son PNG contient le morceau de dégradé
de la carte pris en `26,232`. Si la carte ou l'icône bouge, corriger
`SLOT_HOME_WX` dans `gen_images.py` et régénérer, sinon un rectangle plus clair
apparaît autour du glyphe. `pLight` est aplatie sur un aplat (`COL_CARD`) : elle
se déplace librement dans la tuile sans qu'il faille régénérer.

## 4. Ce que fait le firmware une fois le `.tft` en place

- **Lumières** (`main/ui/ui.c:home_page_tick`) : `tRoom`/`tLights` reflètent la
  pièce affichée (`rooms.c`) ; `pLight` bascule 65↔66 sur `rooms_lights_on()`.
- **Chauffage** : `tTemp`/`tSet` et les deux anneaux suivent l'entité
  `climate.*` de la pièce affichée (`rooms_climate_state()`, alimentée par
  `ha_worker.c` — poll de secours + push WebSocket). Tant qu'aucun état n'est
  connu, ou si l'entité est `unavailable`, `tTemp`/`tSet` affichent `--` et
  seules les pistes grises des anneaux sont dessinées.
- **Repaint des anneaux** : une rafale de `cirs` s'applique à la page affichée
  et ne survit ni à un changement de page ni à un cycle veille/réveil.
  `home_arcs_paint()` n'est donc appelée que sur changement de valeur, à
  l'entrée sur `home` et au réveil (`s_home_needs_repaint` dans `ui.c`) — jamais
  à chaque tick. Si le débit UART négocié (`nextion_current_baud()`) est
  inférieur à 57600 bauds, la rafale est sautée pour ne pas geler la tâche `ui`
  plusieurs secondes ; seuls les textes restent à jour dans ce cas.

## 5. Vérification après upload

Incrémenter `vSpec.val` sur la page `boot` avant de compiler le `.tft`, sinon le
firmware croira la spec inchangée. Puis, panneau alimenté :

```
curl -X POST --data-urlencode "cmd=get home.tClock.txt" http://<panneau>/nx   # 70 … = présent, 1A = absent
curl -X POST --data-urlencode "cmd=home.pWx.pic=60"     http://<panneau>/nx   # doit afficher la pluie
curl -X POST --data-urlencode "cmd=get home.pWx.pic"    http://<panneau>/nx   # → 60
curl -X POST --data-urlencode "cmd=home.pLight.pic=66"  http://<panneau>/nx   # ampoule allumée
curl -X POST --data-urlencode "cmd=home.tWxT.txt=\"24°\"" http://<panneau>/nx # pas de pavé bleu autour → sta/picc bons
curl -X POST --data-urlencode "cmd=cirs 360,130,4,64534" http://<panneau>/nx  # `cirs` existe bien sur cette dalle
```

Si `pWx` affiche la mauvaise icône, une ressource a été importée hors ordre :
réimporter le bloc complet, ne pas compenser côté firmware. Si `cirs` est
refusé (retour `1A`/`1B`), voir la note ci-dessous : `nx_draw_arc()` suppose que
la primitive existe, ce qui n'a pas été vérifié sur le panneau réel avant ce
lot.

## 6. Rappel : coût du fond plein écran

`01_bg_home.png` fait ~55 Ko en PNG mais sera stocké **décompressé** dans le
`.tft` — de l'ordre de 300 Ko en RGB565, soit ~30 s d'upload à 11 Ko/s pour
cette seule page. Relever le chiffre réel dans le panneau Output de l'Editor
après l'import. S'il se confirme, le repli documenté dans `HMI-SPEC.md` tient
toujours : ne garder en Picture que la carte météo (la seule zone réellement
dégradée) et redessiner les parties plates par `fill`/`line` dans le
*Preinitialize Event* de la page.

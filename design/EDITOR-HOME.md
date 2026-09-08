# Montage de la page `home` dans Nextion Editor

Recette pas à pas pour construire la page 1 (`home`) à partir des PNG de
`design/images/out/` (générés par `design/images/gen_images.py`). Complète
`design/HMI-SPEC.md`, qui reste la source de vérité pour les noms d'objets et
l'allocation des `pic` id.

Tout est en **480×320 paysage**. Les coordonnées ci-dessous sont exactes : le
fond `01_bg_home.png` a été dessiné avec elles, et les icônes `pWx` (`26,232`)
et `pLight` (`16,99`) sont aplaties sur le morceau de dégradé qu'elles
recouvrent — les déplacer casse le rendu.

**Les ~32 px de droite de la dalle sont masqués par le cadre du boîtier.** La
largeur utile est donc **448 px**, pas 480 : rien d'utile ne doit dépasser
`x = 440` (marge de page de 8 px, comme à gauche). Les images gardent leur
taille native 480×320 — c'est la mise en page qui se replie (`MASK_R`/`WU` dans
`design/images/gen_images.py`). Les positions ci-dessous en tiennent déjà
compte.

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
`mCfg` (412,12), la tuile Lumières — carte arrondie (8,52)→(215,190) **en
dégradé jaune**, icône et libellé « Lumières » gravés en (16,60) —, la tuile
Chauffage — carte arrondie (232,52)→(440,190) **en dégradé orange**, icône et
libellé « Chauffage » gravés en (244,60) —, et la carte météo en dégradé bleu
marine (8,196)→(440,314) avec son icône et son titre « Météo ».

Les trois tuiles (Lumières, Chauffage, Météo) suivent la recette d'accent du P4
(`card()` de `ha-panel/main/ui/ui_common.c`) : `mix(accent, COL_CARD,
UI_CARD_ACCENT_W)` en haut, fondu vers `COL_CARD` en bas — l'accent de la
tuile Météo est `COL_NAVY` (bleu marine), aligné sur le rendu réel de la tuile
Météo de l'accueil côté ha-panel, pas sur le dégradé `SKY_DAY_T/B` de la page
`weather` dédiée. Le poids est lu dans `ui_common.h` à la génération, comme la
palette. Conséquence pour l'Editor : **tout Text posé sur une tuile doit être
en `sta = crop image`, `picc = 1`** (voir plus bas), sinon un aplat `COL_CARD`
trahit un rectangle sur le dégradé.

**Les anneaux de chauffage ne sont PAS dans le fond.** Ils portent une valeur
(température mesurée / consigne) et sont dessinés à l'exécution par le
firmware (`nx_draw_arc()`, `main/ui/nx_draw.c`, appelé depuis
`home_arcs_paint()` dans `main/ui/ui.c`) — en chapelet de disques `cirs`,
Nextion n'ayant pas de primitive d'arc. Géométrie, à ne rien poser dessus :
centre `(290,138)` — décalé à **gauche** de la tuile et **abaissé** sous le
titre « Chauffage » (icône + libellé, `y≈52..80`) —, anneau extérieur (mesure)
rayon 40 épaisseur 8, anneau intérieur (consigne) rayon 25 épaisseur 6, plage
0..50 °C sur 270° à partir de 135°. Emprise : x 246–334, y 94–182, avec marge
sur les 4 bords de la tuile (232–440 / 52–190) et sous le titre (gap ~14 px).

Plus petit et décalé par rapport à la jauge du panneau P4 (dont il reprend le
principe) : **`tTemp`/`tSet` sont posés à DROITE de l'anneau, pas dedans** —
voir tableau ci-dessous. Un ancien montage centré dans le trou débordait du
rayon sûr (coin de texte à ~38 px pour un bord intérieur sûr à ~30 px) ; le
Nextion n'ayant pas de framebuffer partagé (contrairement au P4/LVGL), chaque
réécriture de texte — y compris la resynchro périodique de `home_page_tick()`,
même sans changement de valeur — repeignait son rectangle et effaçait
l'anneau dessous. Si la tuile Chauffage bouge dans l'Editor, corriger
`HOME_ARC_*` dans `ui.c` en conséquence.

## 2. Composants, dans l'ordre de création

Le firmware n'adresse les objets **que par leur nom** (`home.tClock.txt=…`,
`get home.pWx.pic`) : pour les onze premiers, l'ordre de création n'a aucune
importance, il ne fait que rendre la relecture plus simple.

Les **hotspots** sont l'exception, et pour une raison de protocole : un appui
remonte la trame `0x65 page cid event` (`main/bsp/nextion.c:504`), qui ne
transporte pas de nom — seulement deux entiers. `main/ui/nx_nav.def` (navigation)
et `main/ui/nx_actions.def` (actions hors navigation, ex. bascule des lumières)
doivent donc mapper ces `cid`. L'Editor les attribue par rang de création dans
la page, d'où la consigne : créer les onze objets pilotés d'abord, les cinq
hotspots ensuite, ce qui donne 12..16 — **`mLightToggle` (bascule des lumières
de la pièce) doit être créé en DERNIER**, après `mLight`, pour se retrouver
au-dessus de lui dans l'empilement et intercepter le tactile sur l'ampoule
plutôt que de laisser `mLight` naviguer vers la page Lumières à cet endroit.

| # | `cid` | `objname` | Type | x | y | w | h | Police | `pco` | Alignement |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | — | `tClock` | Text | 14 | 4 | 150 | 44 | f2 | 61342 | gauche / centré vert. |
| 2 | — | `tDate` | Text | 220 | 14 | 165 | 20 | f0 | 35988 | droite / centré |
| 3 | — | `pLight` | Picture | 16 | 99 | 64 | 64 | — | — | `pic` = 65 |
| 4 | — | `tRoom` | Text | 88 | 126 | 128 | 26 | f1 | 65535 | gauche / centré |
| 5 | — | `tLights` | Text | 88 | 156 | 128 | 26 | f0 | 35988 | gauche / centré |
| 6 | — | `tTemp` | Text | 344 | 118 | 90 | 26 | f1 | 62735 | centré / centré |
| 7 | — | `tSet` | Text | 344 | 148 | 90 | 18 | f0 | 60231 | centré / centré |
| 8 | — | `pWx` | Picture | 26 | 232 | 64 | 64 | — | — | `pic` = 56 |
| 9 | — | `tWxT` | Text | 100 | 232 | 160 | 56 | f2 | 61342 | gauche / centré |
| 10 | — | `tWxC` | Text | 100 | 290 | 250 | 20 | f0 | 61342 | gauche / centré |
| 11 | 10 | `mLight` | Hotspot | 0 | 36 | 220 | 156 | — | — | — |
| 12 | 11 | `mHeat` | Hotspot | 224 | 36 | 224 | 156 | — | — | — |
| 13 | 12 | `mWx` | Hotspot | 8 | 196 | 432 | 118 | — | — | — |
| 14 | 13 | `mCfg` | Hotspot | 400 | 0 | 48 | 36 | — | — | — |
| 15 | 14 | `mLightToggle` | Hotspot | 16 | 99 | 64 | 64 | — | — | — |

**Les `cid` 10..13 ci-dessus sont ceux du `.HMI` réellement monté au 09/2026** —
l'ancienne barre de navigation basse `p0..p3`/`t0..t3` a été retirée, ce qui a
renuméroté les hotspots. `main/ui/nx_nav.def` doit rester synchrone avec cette
colonne `cid` ; en cas de doute, se fier au fichier, pas à ce document, et
corriger l'un ou l'autre pour qu'ils concordent.

**`mLightToggle` (cid 14) n'est pas encore monté dans l'Editor** : sa valeur
est *prédite* par le rang de création (5e hotspot de la page), pas vérifiée
sur un `.tft` réel. Après l'avoir créé, relever le vrai `cid` via `touches`
dans `/status` (appui sur l'ampoule) et corriger `main/ui/nx_actions.def`
(`NX_ACT(NXP_HOME, 14, ACT_LIGHT_TOGGLE)`) si besoin.

**La géométrie (x/y/w/h) ci-dessus a été recalculée pour `MASK_R=32`** (voir
`gen_images.py`), mais **pas encore réappliquée dans l'Editor** : le `.HMI`
actuel porte toujours les positions de l'ancien `MASK_R=25`. Prochaine ouverture
de l'Editor pour cette page : redéplacer les objets/hotspots (surtout `mWx`,
`mHeat`, `mLight`, `mCfg`, dont le bord droit suit le fond) sur ces nouvelles
valeurs, puis vérifier `touches` dans `/status` comme d'habitude.

`pco` est donné en RGB565 décimal, déjà arrondi à la grille 565 comme les
images (`COL_TXT` 61342, `COL_TXT_DIM` 35988, `COL_HEAT_SOFT` 62735,
`COL_HEAT` 60231 — ces deux dernières valeurs doivent rester identiques à
`COL_HEAT_SOFT_565`/`COL_HEAT_565` dans `main/ui/ui.c`, sinon le texte et
l'anneau qu'il annote n'ont pas la même couleur).

`tTemp`/`tSet` sont posés à **droite** de l'anneau (x≥344), pas dans son trou :
l'anneau occupe x:246–334, donc aucune contrainte de largeur pour ces deux
champs — contrairement à un ancien montage centré dans le trou, qui débordait
du rayon sûr et se faisait effacer par la resynchro périodique du texte (voir
§1). Garder une marge malgré tout (x+w ≤ 432, bord droit de la tuile − 8) pour
ne pas déborder de la carte. **Avec `MASK_R=32`, `tTemp`/`tSet` (x=344, w=90,
x+w=434) dépassent cette marge de 2 px** — à corriger dans l'Editor en réduisant
légèrement `w` (ou en décalant `x`) lors du prochain montage de `home`.

### `vscope` = **global** sur tous les objets écrits par le firmware

`tClock`, `tDate`, `tRoom`, `tLights`, `tTemp`, `tSet`, `pWx`, `tWxT`, `tWxC`,
`pLight` : `vscope` = **`global`** (déjà le cas dans le `.HMI`). C'est ce qui
les rend joignables sous `home.<objet>.<attr>` depuis n'importe quelle page —
donc sondables au boot. Règle valable pour toute page ajoutée ensuite, voir
`HMI-SPEC.md` § `vscope`. Les hotspots peuvent rester `local`.

### Fond de chaque composant : `sta` = **Crop Image**, `picc` = **1**

Pour **tous** les Text ci-dessus, mettre `sta` à `crop image` et `picc` à `1`.
C'est ce qui fait qu'un texte se redessine sur les pixels réels du fond au lieu
d'un rectangle de couleur. Indispensable sur la carte météo (`tWxT`, `tWxC`) où
le fond est un dégradé ; sur le noir plat (`tClock`, `tDate`, `tTemp`, `tSet`…)
ça ne se voit pas, mais autant garder la même règle partout — sinon un
déplacement de 2 px trahit le pavé.

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

`mLightToggle` fait exception à la navigation : son `cid` n'apparaît pas dans
`nx_nav.def` mais dans `main/ui/nx_actions.def`
(`NX_ACT(NXP_HOME, 14, ACT_LIGHT_TOGGLE)`), qui appelle
`rooms_toggle_current()` (`main/ui/rooms.c`) — bascule toutes les lumières de
la pièce affichée (`rooms_current()`) : si l'une au moins est allumée, les
éteint toutes, sinon les allume toutes. Mêmes réglages Editor que les autres
hotspots (Send Component ID sur Touch Press, pas de `page N`) : c'est
`drain_touches()` (`main/ui/ui.c`) qui fait la différence entre navigation et
action selon la table où le `(page, cid)` est trouvé.

## 3. Images utilisées par cette page

| `pic` | Fichier | Objet | Remarque |
|---|---|---|---|
| 1 | `01_bg_home.png` | fond de page | 480×320, tuiles + carte météo, sans les anneaux |
| 56..64 | `56_home_wx64_*.png` | `pWx` | ordre `sunny, clear-night, partlycloudy, cloudy, rainy, snowy, lightning, windy, fog` — le firmware écrit `56 + index` |
| 65 / 66 | `65_home_light64_off.png` / `66_..._on.png` | `pLight` | aplaties sur le dégradé de la tuile Lumières pris en `16,99` |

**`pWx` et `pLight` sont tous deux liés à leur position** : leur PNG contient le
morceau exact de dégradé qu'ils recouvrent — la carte météo en `26,232` pour
`pWx`, la tuile Lumières en `16,99` pour `pLight`. Si la carte, la tuile ou
l'icône bouge, corriger `SLOT_HOME_WX` / `SLOT_HOME_LIGHT` dans `gen_images.py`
et régénérer, sinon un rectangle de la mauvaise teinte apparaît autour du
glyphe. (`pLight` se déplaçait librement tant que la tuile Lumières était un
aplat `COL_CARD` ; ce n'est plus vrai depuis qu'elle est en dégradé.)

## 4. Ce que fait le firmware une fois le `.tft` en place

- **Lumières** (`main/ui/ui.c:home_page_tick`) : `tRoom`, `tLights` et `pLight`
  reflètent tous les trois la **pièce affichée** (`rooms_current()`) —
  `tLights`/`pLight` via `rooms_lights_on_in(room)`/`rooms_light_count(room)`
  (`rooms.c`), pas `rooms_lights_on()`/`rooms_lights_total()` qui restent
  volontairement globales (toutes pièces confondues, utilisées par `/status`
  et le log de boot, pas par `home`). `pLight` bascule 65↔66 selon que la
  pièce affichée a au moins une lumière allumée. Un appui sur l'ampoule
  (`mLightToggle`, `ACT_LIGHT_TOGGLE`) bascule les lumières de cette même
  pièce (`rooms_toggle_current()`) : si l'une au moins est allumée, les
  éteint toutes, sinon les allume toutes — cohérent avec ce que `pLight`
  affiche.
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

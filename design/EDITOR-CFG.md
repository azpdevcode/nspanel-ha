# Montage de la page `cfg` dans Nextion Editor

Recette pas à pas pour construire la page 5 (`cfg`) à partir de
`design/images/out/05_bg_cfg.png` (généré par `design/images/gen_images.py`).
Complète `design/HMI-SPEC.md`, qui reste la source de vérité pour les noms
d'objets — voir aussi `design/EDITOR-HOME.md` pour la méthode (ce document en
reprend la structure).

Tout est en **480×320 paysage**, mais les **~25 px de droite sont masqués par le
cadre du boîtier** : la largeur utile est de **455 px** et rien d'utile ne doit
dépasser `x = 447`. L'image de fond garde sa taille native ; c'est la mise en
page qui se replie (`MASK_R`/`WU` dans `design/images/gen_images.py`). Les
coordonnées ci-dessous en tiennent déjà compte.

## 0. Prérequis

1. Projet Editor en **NX4832F035_011** (Discovery 3,5"), orientation
   **paysage**.
2. Polices `.zi` **générées en `iso-8859-1`** (jamais utf-8, cf. HMI-SPEC) :
   `f0` = 16 px, `f1` = 24 px, `f2` = 48 px — déjà en place si `home` est
   montée.
3. Ressource Picture **5 remplacée** (clic droit → *Replace Picture*, jamais
   *Add* — un *Add* décalerait tous les id 6..66 qui suivent) par le nouveau
   `05_bg_cfg.png`.
4. Page nommée exactement `cfg`, en **sixième position** (index 5, cf.
   `main/bsp/nx_pages.def`) — c'est déjà le cas si elle existe.

## 1. Nettoyage de l'existant

La page `cfg` contient aujourd'hui une ébauche : `tRoom` (id 1, `52,66
136×34`, `sta=1` couleur unie) et `bOta` (id 2, Button `12,240 144×64`). Leur
géométrie ne correspond plus au nouveau fond et `sta=1` laisserait un pavé de
couleur unie par-dessus la carte arrondie. **Supprimer les deux** et repartir
de zéro.

## 2. Attributs de la page

| Attribut | Valeur |
|---|---|
| `objname` | `cfg` |
| `sta` | `image` |
| `pic` | `5` (`05_bg_cfg.png`) |

Ce qui est **déjà dans le fond** et ne doit pas être recréé : la flèche retour
en haut à gauche, la carte « PIÈCE » avec ses deux flèches, les libellés
« LUMINOSITÉ » et « VEILLE » (les sliders eux-mêmes n'ont pas de rail dessiné —
leur `bco` en tient lieu, voir tableau ci-dessous), les trois cartes de
diagnostic (SSID/IP/HA), les trois cartes d'action (OTA/TFT/Reboot) et la
carte de version.

## 3. Composants, dans l'ordre de création

Comme sur `home` : le firmware n'adresse les objets que par leur nom pour les
neuf premiers (ordre de montage indifférent), mais les **hotspots** remontent
un `cid` numérique nu (`0x65 page cid event`) — les créer en dernier pour
qu'ils soient contigus, ce qui simplifie leur relevé.

| # | cid prévu | `objname` | Type | x | y | w | h | Police | `pco` | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | — | `tRoom` | Text | 66 | 26 | 270 | 20 | f1 | 61342 | gauche / centré |
| 2 | — | `tDim` | Text | 355 | 60 | 84 | 20 | f0 | 61342 | droite / centré |
| 3 | — | `tSleep` | Text | 355 | 106 | 84 | 20 | f0 | 61342 | droite / centré |
| 4 | — | `hDim` | Slider (h) | 16 | 86 | 423 | 18 | — | 62963 | `minval`=10, `maxval`=100 |
| 5 | — | `hSleep` | Slider (h) | 16 | 132 | 423 | 18 | — | 15335 | `minval`=0, `maxval`=300 |
| 6 | — | `tSsid` | Text | 70 | 160 | 369 | 20 | f0 | 61342 | gauche / centré |
| 7 | — | `tIp` | Text | 70 | 191 | 369 | 20 | f0 | 61342 | gauche / centré |
| 8 | — | `tHa` | Text | 70 | 222 | 369 | 20 | f0 | 61342 | gauche / centré |
| 9 | — | `tVer` | Text | 14 | 298 | 426 | 16 | f0 | 23261 | gauche / centré |
| 10 | 10 | `bRoomPrev` | Hotspot | 353 | 6 | 46 | 46 | — | — | — |
| 11 | 11 | `bRoomNext` | Hotspot | 393 | 6 | 46 | 46 | — | — | — |
| 12 | 12 | `bOta` | Hotspot | 16 | 252 | 134 | 40 | — | — | — |
| 13 | 13 | `bTft` | Hotspot | 160 | 252 | 134 | 40 | — | — | — |
| 14 | 14 | `bReboot` | Hotspot | 304 | 252 | 134 | 40 | — | — | — |
| 15 | 15 | `bRet` | Hotspot | 0 | 4 | 52 | 48 | — | — | — |

`pco` en RGB565 décimal, arrondis depuis la palette (`COL_TXT` 61342,
`COL_TXT_FAINT` 23261 pour `tVer`, `COL_LIGHTS` 62963 pour la barre de `hDim`,
`COL_HOME` 15335 pour `hSleep`). Sur les deux Sliders, régler `bco` (couleur du
rail au repos) à `COL_CARD2` **6406** — même valeur que `COL_CARD2_565` dans
`main/ui/ui.c` — c'est ce qui remplace visuellement le rail qu'on aurait pu
dessiner dans le fond.

### Fond de chaque composant : `sta` = **Crop Image**, `picc` = **5**

Comme sur `home` (`design/EDITOR-HOME.md` § « Fond de chaque composant ») :
pour **tous** les Text ci-dessus, `sta` = `crop image`, `picc` = `5`. Sans ça,
un texte réécrit laisse un rectangle de couleur unie par-dessus la carte
arrondie du fond.

### Valeurs initiales (`txt`)

Tout à vide, sauf éventuellement `tVer` (« … » en attendant). Le firmware
écrit la page dans les 250 ms qui suivent son affichage
(`main/ui/ui.c:cfg_page_tick`).

## 4. Événements tactiles

- **Hotspots** (`bRoomPrev`, `bRoomNext`, `bOta`, `bTft`, `bReboot`, `bRet`) :
  cocher **Send Component ID** sur *Touch Press Event* uniquement, aucun code
  Nextion, et surtout **pas de `page N`** dans l'Editor — la navigation
  appartient au firmware (`main/ui/nx_nav.def`), sinon un `.tft` partiel
  court-circuiterait `nx_page_present()` (même consigne que sur `home`).
- **Sliders** (`hDim`, `hSleep`) : cocher **Send Component ID** sur *Touch
  Release Event* **uniquement** (pas Press). Le firmware ne peut pas lire un
  Slider en glissant — il relit `cfg.hDim.val`/`cfg.hSleep.val` en réponse à
  l'événement de relâche (`nx_get_num()`, `main/bsp/nextion.c`) ; réagir dès
  l'appui donnerait une valeur instable.

## 5. Ce que fait le firmware une fois le `.tft` en place

- `main/ui/ui.c:cfg_page_tick()` maintient `tRoom`, `tDim`, `tSleep`, `tSsid`,
  `tIp`, `tHa`, `tVer` à jour, au même rythme que `home_page_tick()` (écriture
  seulement sur changement, resynchro forcée toutes les ~20 s).
- `hDim`/`hSleep` (`.val`) ne sont écrits par le firmware **qu'à l'entrée sur
  la page** (`s_cfg_needs_sync`) — jamais à chaque tick, sinon le curseur
  sauterait sous le doigt pendant un glissé.
- `bRoomPrev`/`bRoomNext` appellent `rooms_set_current()` **et**
  `settings_save_default_room()` : le choix est immédiat sur `home` et survit
  au redémarrage (persisté par nom, cf. `main/ui/settings.h`).
- `bOta`/`bTft` n'ont pas de clavier tactile pour saisir une URL : ils
  affichent l'adresse IP du panneau dans `tVer` pendant quelques secondes,
  l'invite étant de lancer la mise à jour depuis le portail web
  (`http://<ip>/`, `POST /ota` ou `/tft`).
- `bReboot` appelle `esp_restart()` après une courte pause (laisse partir une
  écriture NVS en cours).
- `bRet` revient à `home` via `main/ui/nx_nav.def` — le retour automatique
  après inactivité existe aussi en garde-fou (120 s sur `cfg`, contre 30 s sur
  les autres pages secondaires).

## 6. Vérification après upload

Incrémenter `boot.vSpec.val` (voir `main/bsp/nx_objects.def` / valeur actuelle
dans le `.HMI`) avant de compiler le `.tft`, sinon le firmware croit la spec
inchangée et les nouveaux objets restent « absents » — toute écriture dessus
devient un no-op silencieux. Puis, panneau alimenté :

```
curl -X POST --data-urlencode "cmd=get cfg.tRoom.txt"  http://<panneau>/nx   # 70 … = présent
curl -X POST --data-urlencode "cmd=get cfg.hDim.val"   http://<panneau>/nx   # 71 … = lisible
curl -X POST --data-urlencode 'cmd=cfg.tSsid.txt="Livebox"' http://<panneau>/nx  # pas de pavé uni autour → sta/picc bons
```

**Relevé des `cid` réels** — la colonne « cid prévu » n'est qu'une prédiction
issue de l'ordre de création ci-dessus : toucher chaque hotspot et chaque
Slider (relâche comprise), puis

```
curl http://<panneau>/status | jq .touches
```

et reporter les valeurs réelles dans `main/ui/nx_actions.def` (actions) et
`main/ui/nx_nav.def` (`bRet`). C'est le seul endroit à corriger si un composant
est supprimé puis recréé dans l'Editor.

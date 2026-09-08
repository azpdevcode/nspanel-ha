# Spécification HMI — nspanel-ha

Référence pour le montage du `.HMI` dans Nextion Editor. Source de vérité
couplée à `main/bsp/nx_objects.def` et `main/bsp/nx_pages.def` — toute
modification ici doit se refléter dans ces deux fichiers (et réciproquement).

## Général

- Écran : Nextion Discovery NX4832F035, résistif.
- **Orientation : 480×320 paysage** — résolution native de la dalle, et ce que
  le projet Editor utilise déjà. Figé : toutes les positions des pages ci-dessous
  et toutes les tailles d'images (`design/PROMPT-IMAGES.md`) en dépendent.
- **Les ~32 px de droite sont masqués par le cadre du boîtier.** Ces colonnes
  existent dans la dalle, dans les images et dans les coordonnées Nextion, mais
  ne sont jamais visibles. Les images gardent donc leur **taille native
  480×320** — les redimensionner désalignerait tout — et c'est la **mise en
  page** qui se replie sur une largeur utile de **448 px** (`MASK_R` / `WU`
  dans `gen_images.py`), avec la même marge de 8 px à droite qu'à gauche : rien
  d'utile au-delà de `x = 440`. Vaut aussi pour les composants posés dans
  l'Editor (voir `EDITOR-HOME.md`, `EDITOR-CFG.md`) et pour le firmware, dont
  le centre des anneaux de `home` (`HOME_ARC_CX`, `main/ui/ui.c`) suit celui de
  la tuile Chauffage recadrée. Seule exception assumée : le dégradé plein écran
  de `weather` est peint sur les 480 px, pour qu'un cadre un peu moins couvrant
  que prévu ne révèle pas de liseré noir.
- **Reparse mode : rien à faire dans l'Editor.** Ce n'est pas une option de
  compilation mais un état d'exécution (`recmod`) activé par le programme du
  `.tft` chargé — celui d'origine Sonoff le réaffirme en continu. C'est le
  firmware ESP32 qui l'en sort (`exit_reparse_mode()` dans `bsp/nextion.c`),
  au boot et avant chaque upload. Ne pas chercher de case à cocher.
- **`vscope` : mettre `global` sur tout objet listé dans `nx_objects.def`.**
  Le firmware adresse ces objets par leur chemin complet
  `<page>.<objet>.<attribut>`, construit à partir de la page déclarée dans
  `nx_objects.def` (`qname()` dans `bsp/nextion.c`), et il les sonde tous au
  boot depuis la page affichée à ce moment-là. Or un nom qualifié ne suffit
  pas : **le préfixe de page ne rend joignable qu'un composant `global`.** Un
  composant `local` (le défaut de l'Editor) ne répond que pendant que sa page
  est affichée, qualifié ou non — `0x1A` sinon. Mesuré sur la cible : depuis
  `home`, `get cfg.tRoom.txt` → `1A` tant que `cfg.tRoom` est `local`, `70 …`
  une fois passé `global`. Les hotspots de navigation ne sont pas concernés
  (ils ne sont ni sondés ni écrits), et `vis` reste l'exception inverse : il
  refuse un nom qualifié (`0x02`) et ne vaut que pour la page affichée.
- Symptôme si un jour ça régresse : `objects_present` reste à la seule page
  affichée après un sondage complet, `pages` marque `false` toutes les autres,
  et la navigation vers elles devient un no-op silencieux (`nx_page()` refuse
  une page dont l'ancre est absente). Se diagnostique en deux appels —
  `curl -X POST --data-urlencode "cmd=get home.tClock.txt" http://<panneau>/nx`
  doit renvoyer `70 …`. Un `1A` sur `<page>.<objet>` alors que `page <page>`
  puis `get <objet>.<attr>` répond : c'est un `vscope` resté `local`, pas un
  objet absent.
- **Biais tactile : défaut matériel connu du NSPanel EU, pas un problème de
  résolution ni de calibration à deviner page par page.** Le bezel/vitre du
  boîtier masque une bande à droite de l'écran (~25-32 px, cohérent avec
  `MASK_R=32` dans `design/images/gen_images.py`, aligné sur la correction
  `lcd_dev … 0020` ci-dessous — `0x20` = 32), et le numériseur résistif
  garde une calibration d'usine qui ne tient pas compte de cette bande — d'où
  un `x` rapporté systématiquement plus grand que le point réellement pressé,
  l'écart croissant vers la droite (et variant aussi avec `y`, la pression du
  cadre n'étant pas uniforme). **Documenté et corrigé par la communauté
  NSPanel** (wiki `masto/NSPanel-Demo-Files`, issue `marcfager/nspanel-mf#5`) :
  une seule ligne dans `Program.s`, **avant `page 0`** :
  ```
  lcd_dev fffb 0002 0000 0020
  ```
  Corrige le décalage à la source dans le firmware Nextion — hotspots et
  Sliders (`hDim`/`hSleep`) en une fois, sans mesure ni compensation par page.
  Une session précédente (2026-09-08) avait d'abord élargi/décalé les hotspots
  de `cfg` à la main pour absorber le biais : **abandonné**, remplacé par ce
  correctif — ne pas réintroduire ce genre de compensation géométrique par
  page, la vraie cause est réglée une fois pour toutes ici.
  `touch_j` (calibration interactive) reste un outil valide, mais **seulement
  placé dans l'événement Page Preinitialize d'un projet dédié à la
  calibration**, jamais envoyé en commande série au runtime (constaté sans
  effet visible même avec `ui_freeze()` actif, cf. ci-dessous — ce n'est pas
  une resynchronisation du firmware qui l'empêchait, la doc Nextion précise
  que cet événement ne s'exécute qu'une fois, au tout premier boot du projet
  compilé).
  Pour toute manipulation manuelle de l'écran via `/nx` (mesure `sendxy`,
  test d'une instruction Nextion...), geler d'abord `ui_task` pour ne pas
  mélanger le résultat avec sa resynchronisation périodique (toutes les 20 s,
  `resync_due()` dans `main/ui/ui.c`) :
  ```
  curl -d "freeze=1" http://<panneau>/nx           # suspend ui_task
  curl -d "cmd=sendxy=1" http://<panneau>/nx       # active le rapport brut
  curl -d "cmd=&window=3000" http://<panneau>/nx   # écoute 3 s, PENDANT
                                                     # qu'on touche l'écran
  curl -d "cmd=sendxy=0" http://<panneau>/nx       # désactive en fin de mesure
  curl -d "freeze=0" http://<panneau>/nx           # reprend ui_task
  ```
  Chaque appui renvoie deux trames `67 xH xL yH yL ev FF FF FF` (`0x67`,
  x/y en little-endian, `ev`=1 pression puis 0 relâche) — ce sont les
  coordonnées vues par l'écran, à comparer à la position visuelle du repère
  touché (géométrie dans `EDITOR-*.md` ou `design/images/gen_images.py`).
  Ne mesurer que sur des icônes/repères déjà validés comme non destructifs
  (une carte OTA/TFT/Reboot mal calée redéclenche l'action pour de vrai — vécu
  le 2026-09-08, redémarrage intempestif du panneau en pleine mesure).
- Polices `.zi` (chasse fixe) : `f0` 16 px, `f1` 24 px, `f2` 48 px.
  **Générer en encodage `iso-8859-1`, jamais `utf-8`.** C'est l'encodage qui
  fait la taille, pas les cases « Latin base / Latin-1 » cochées : en mode
  utf-8 le générateur produit une table couvrant les 65 536 codes du BMP, dont
  ~65 300 vides à ~10 octets pièce, soit un plancher de ~670 Ko par police
  quelle que soit sa hauteur (mesuré : `f1` 24 px = 671 Ko, `f2` 48 px = 700 Ko,
  4 % d'écart pour 4× la surface de glyphe). En `iso-8859-1` : 224 codes, ~5 Ko
  en 16 px, ~50 Ko en 48 px — accents français et `°` compris. L'encodage d'un
  `.zi` se lit à l'offset 0x2c du fichier, le nombre de codes à l'offset 0x0c.
  Le panneau Output de l'Editor donne `Total size of font:` — viser < 100 Ko.
- Corollaire : l'écran attend **un octet par caractère**, Home Assistant renvoie
  de l'UTF-8. La conversion se fait dans `latin1_escape_into()` (`bsp/nextion.c`),
  sur le chemin de `nx_set_txt()` ; hors Latin-1 → `?`.
- Icônes : pas de police d'icônes — glyphes Material exportés en PNG, ajoutés
  en ressources Picture, affichés par `pic`/`picq`. Le jeu complet (fonds de
  page compris) est **généré** par `design/images/gen_images.py`, pas dessiné à
  la main — voir « Ressources Picture » ci-dessous.
- Palette : **`ui_common.h` du panneau P4 fait foi**, et `gen_images.py` la lit
  à la génération (`HA_PANEL_UI_COMMON` pour pointer ailleurs ; sans le dépôt
  voisin, une copie de secours prend le relais). Aucune teinte n'est donc
  recopiée ici — pour l'Editor, relever la valeur voulue dans ce fichier.
  Chaque couleur est ramenée à la grille RGB565 **par arrondi** avant usage :
  une couleur exacte traverse le tramage sans erreur, donc sans bruit sur les
  aplats. Attention, `>>3` (troncature) décale toute la palette vers le sombre —
  `COL_BG` 0x0d1117 donne `0x1083` en arrondi, pas `0x0841`.

## Ressources Picture

Générées par `python3 design/images/gen_images.py` (sortie `design/images/out/`,
~55 Ko au total). PNG 24 bits **sans alpha** : la série Discovery ignore la
transparence, chaque image est donc déjà aplatie sur son fond (`#0d1117` pour
les pages, `#161c24` pour les icônes posées sur une carte).

L'id d'une ressource **n'est pas un champ que l'on choisit** : c'est son rang
dans la liste Picture de l'Editor, attribué à l'ajout. Les fichiers sont donc
préfixés de leur id — **les ajouter dans l'ordre alphabétique, en bloc**. Les
`NN_reserved.png` de 1×1 px sont des gardes : les supprimer décale tout ce qui
suit et le firmware affiche la mauvaise image.

| id | contenu | taille |
|---|---|---|
| 0–5 | un fond par page, dans l'ordre de `nx_pages.def` | 480×320 |
| 6–9 | garde | 1×1 |
| 10–18 | icône météo grand format (`pNow`, `pWx`) | 64×64 |
| 19 | garde | 1×1 |
| 20–28 | icône météo prévision (`pD0`..`pD3`) | 40×40 |
| 29 | garde | 1×1 |
| 30–55 | icônes d'interface, par paires inactive/active | 32×32 |
| 56–64 | icône météo de `home.pWx` (aplatie sur la carte dégradée) | 64×64 |
| 65–66 | badge circulaire de la tuile Lumières (`pLight`), éteint/allumé — aplati sur le dégradé de la tuile, centré (voir `SLOT_HOME_LIGHT` dans `gen_images.py`) | 88×88 |

Les deux blocs météo suivent le **même ordre de conditions** — `sunny`,
`clear-night`, `partlycloudy`, `cloudy`, `rainy`, `snowy`, `lightning`,
`windy`, `fog` — pour que le firmware calcule `base + index` (`10 + i` en grand
format, `20 + i` en prévision) ; `fog` sert de repli pour `exceptional` et
l'inconnu. Le mapping condition → index est celui de `weather_icon()` du
panneau P4.

**Coût réel dans le `.tft` : à mesurer.** Les ~590 Ko de PNG ne préjugent de
rien — Nextion stocke une Picture en RGB565 brut, soit ~307 Ko par fond plein
écran, donc potentiellement ~1,8 Mo pour les six et ~3 min par upload à
11 Ko/s. Vérifier sur le panneau Output de l'Editor après avoir ajouté
`01_bg_home.png` seul. Si le chiffre se confirme, ne garder en Picture que les
deux zones réellement dégradées (carte météo de `home`, page `weather`) et
dessiner les parties plates avec des `fill`/`line` dans l'Preinitialize Event
de chaque page — coût nul en ressources.

Faute de canal alpha, une icône posée sur un dégradé est aplatie sur le
**morceau exact de fond qu'elle recouvre** : elle est liée à sa position.
Déplacer `pWx` (`26,232` sur `home`), `pLight` (centré dans la tuile Lumières, voir `SLOT_HOME_LIGHT`), `pNow` (`24,54`) ou
la bande des `pD*` (`y=236`) dans l'Editor oblige à corriger `SLOT_*` dans
`gen_images.py` et à régénérer, sinon un rectangle de la mauvaise teinte
apparaît autour du glyphe.

Les tuiles Lumières et Chauffage de `home` sont elles-mêmes en **dégradé
d'accent** (jaune `COL_LIGHTS`, orange `COL_HEAT`), suivant la recette du
panneau P4 : `mix(accent, COL_CARD, UI_CARD_ACCENT_W)` en haut fondu vers
`COL_CARD` en bas, le poids étant lu dans `ui_common.h` à la génération comme
la palette. Corollaire pour l'Editor : **tout Text posé sur une tuile doit être
en `sta = crop image` avec le bon `picc`**, un `solid color` y laisserait un
rectangle visible sur le dégradé.

Contrôle après upload — la seule preuve que l'allocation tient :
```
curl -X POST --data-urlencode "cmd=home.pWx.pic=14" http://<panneau>/nx   # pluie
curl -X POST --data-urlencode "cmd=get home.pWx.pic" http://<panneau>/nx  # → 14
```
Si l'icône affichée n'est pas la bonne, une ressource a été ajoutée hors ordre :
réimporter le bloc, ne pas compenser côté firmware.

## Variable de révision — `vSpec`

Variable globale (page `boot`), attribut `.val` = révision entière de cette
spec, à **incrémenter à chaque upload**. Le firmware la lit au boot pour
décider s'il doit re-sonder les objets (voir `bsp/nextion.c:nextion_probe`).
Sans elle, le firmware suppose un écran d'origine et n'essaie plus rien.

## Cycle d'itération (vérifié en phase 1)

Aucun moniteur série n'est nécessaire — tout passe par HTTP (cf. « Diagnostic
sans moniteur série » dans le plan de fork) :

1. Modifier l'HMI dans l'Editor, **incrémenter `vSpec.val`**, compiler.
2. Servir le fichier : `python3 -m http.server 8000` dans son dossier.
   Ne jamais recompiler pendant qu'un transfert est en cours — un `.tft` servi
   à moitié réécrit donne un « System data ERROR! » à l'écran (état récupérable :
   il suffit de refaire un upload complet).
3. `curl -X POST --data-urlencode "url=http://<pc>:8000/nspanel.tft" http://<panneau>/tft`
4. Suivre `curl http://<panneau>/status` → `tft_ota.status` donne la progression,
   puis `screen.vspec_rev` et `screen.pages` confirment le sondage.

Compter ~11 Ko/s (115200 bauds) : quelques secondes pour un `.tft` bien dégraissé,
plusieurs minutes si la police n'a pas été restreinte.

## Ordre de montage suggéré

`boot` (+ `vSpec`) → `home` minimal (horloge/date) → `lights` (2 lignes) →
`lights` complet → `climate` → `weather` → `cfg` → tuiles de `home`. Le
firmware tourne à chaque étape intermédiaire (voir « Tolérance au TFT
partiel » dans le plan de fork) — l'ordre réel n'a pas d'importance.

## Pages

### `boot` (page 0)

| Objet | Type | Rôle |
|---|---|---|
| `vSpec` | Variable | révision de la spec (voir ci-dessus) |
| `tStat` | Text | statut WiFi/IP/HA, multi-lignes |
| `jProg` | Progress bar | avancement OTA applicatif/TFT |

### `home` (page 1)

| Objet | Type | Rôle |
|---|---|---|
| `tClock` | Text (f2) | heure |
| `tDate` | Text (f0) | date |
| `pLight` | Picture | badge circulaire de la tuile Lumières (65 éteint / 66 allumé), centré dans la tuile |
| `tTemp` | Text (f1) | température mesurée (entité `climate.*` de la pièce) |
| `tSet` | Text | consigne (idem) |
| `pWx` | Picture | icône météo courante |
| `tWxT` | Text | température météo |
| `tWxC` | Text | condition météo (libellé court) |
| `mLight`, `mHeat`, `mWx`, `mCfg` | Hotspot | navigation (non sondés — absence silencieuse) |

Les deux anneaux de la tuile Chauffage **ne sont pas des objets Nextion** : ils
sont dessinés dynamiquement par le firmware en chapelet de disques (`cirs`,
`main/ui/nx_draw.c`), rien à déclarer ici ni dans l'Editor — voir
`design/EDITOR-HOME.md` pour la géométrie et les couleurs.

### `lights` (page 2)

| Objet | Type | Rôle |
|---|---|---|
| `tRoom` | Text | pièce affichée (ancre de la page) |
| `bRoomPrev`/`bRoomNext` | Button | changer de pièce (hotspots) |
| `bAll` | Button | tout éteindre (hotspot) |
| `tN0`..`tN7` | Text | nom de chaque lumière (jusqu'à 8) |
| `bN0`..`bN7` | Button | bascule (couleur `.pco` reflète l'état) |
| `hN0`..`hN7` | Slider (h) | luminosité (si la lumière la supporte) |

Une ligne sans `hN<i>` reste pilotable par `bN<i>` seul (bascule). Une ligne
sans `bN<i>` n'est simplement pas rendue.

### `climate` (page 3)

| Objet | Type | Rôle |
|---|---|---|
| `tCur` | Text (f2) | température courante |
| `tSet` | Text (f2) | consigne |
| `bUp`/`bDn` | Button | ± consigne (hotspots) |
| `tAction` | Text | état (chauffe / idle / absent…) |
| `bAway`/`bFrost`/`bAuto` | Button | préréglages (hotspots) |

### `weather` (page 4)

| Objet | Type | Rôle |
|---|---|---|
| `pNow`, `tNow`, `tCond` | Picture/Text | conditions actuelles |
| `tHum`, `tWind`, `tPres` | Text | détail (humidité/vent/pression) |
| `pD0`..`pD3`, `tD0`..`tD3`, `tHi0`..`tHi3`, `tLo0`..`tLo3` | Picture/Text | 4 jours de prévision |

### `cfg` (page 5)

| Objet | Type | Rôle |
|---|---|---|
| `tRoom` | Text | pièce par défaut (ancre) |
| `bRoomPrev`/`bRoomNext` | Button | changer la pièce par défaut (hotspots) |
| `hDim` | Slider | luminosité écran |
| `tDim` | Text | valeur courante de `hDim` (« 80 % ») |
| `hSleep` | Slider | délai de veille (0 = désactivée) |
| `tSleep` | Text | valeur courante de `hSleep` (« 60 s » / « désactivée ») |
| `tSsid`, `tIp`, `tHa` | Text | diagnostic réseau/HA |
| `bOta`, `bTft`, `bReboot` | Button | actions (hotspots) |
| `tVer` | Text | version firmware |
| `bRet` | Button | retour à `home` (hotspot) |

Les Sliders `hDim`/`hSleep` doivent cocher **Send Component ID** sur *Touch
Release Event* uniquement (pas Press) : le firmware relit leur `.val` par un
aller-retour `get` en réponse à l'événement, et pendant le glissé la valeur
n'est pas stable — cf. `main/bsp/nextion.c:nx_get_num` et
`design/EDITOR-CFG.md`.

La saisie de l'URL/token HA reste sur le **portail web** (`http://<ip>/`),
pas sur cette page — pas de clavier tactile pour un jeton de 180 caractères.

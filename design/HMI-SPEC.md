# Spécification HMI — nspanel-ha

Référence pour le montage du `.HMI` dans Nextion Editor. Source de vérité
couplée à `main/bsp/nx_objects.def` et `main/bsp/nx_pages.def` — toute
modification ici doit se refléter dans ces deux fichiers (et réciproquement).

## Général

- Écran : Nextion Discovery NX4832F035, résistif.
- **Orientation : 480×320 paysage** — résolution native de la dalle, et ce que
  le projet Editor utilise déjà. Figé : toutes les positions des pages ci-dessous
  et toutes les tailles d'images (`design/PROMPT-IMAGES.md`) en dépendent.
- **Reparse mode : rien à faire dans l'Editor.** Ce n'est pas une option de
  compilation mais un état d'exécution (`recmod`) activé par le programme du
  `.tft` chargé — celui d'origine Sonoff le réaffirme en continu. C'est le
  firmware ESP32 qui l'en sort (`exit_reparse_mode()` dans `bsp/nextion.c`),
  au boot et avant chaque upload. Ne pas chercher de case à cocher.
- **`vscope` : rien à faire, laisser `local`.** Le firmware adresse tout objet
  par son chemin complet `<page>.<objet>.<attribut>`, construit à partir de la
  page déclarée dans `nx_objects.def` (`qname()` dans `bsp/nextion.c`). Vérifié
  sur la cible : sous son nom nu, un composant ne répond que pendant que sa page
  est affichée (`0x1A` sinon) ; qualifié, il répond en lecture comme en écriture
  depuis n'importe quelle page, et la valeur écrite survit à la navigation.
  Seule exception : `vis`, qui refuse un nom qualifié (`0x02`) et ne vaut donc
  que pour la page affichée.
- Symptôme si un jour ça régresse : `objects_present` reste à la seule page
  affichée après un sondage complet. Se diagnostique en deux appels —
  `curl -X POST --data-urlencode "cmd=get home.tClock.txt" http://<panneau>/nx`
  doit renvoyer `70 …`, un `1A` désignant un objet vraiment absent.
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
| 65–66 | ampoule de la tuile Lumières (`pLight`), éteinte/allumée — aplatie sur `COL_CARD` | 64×64 |

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
Déplacer `pWx` (`26,232` sur `home`), `pNow` (`24,54`) ou la bande des `pD*`
(`y=236`) dans l'Editor oblige à corriger `SLOT_*` dans `gen_images.py` et à
régénérer, sinon un rectangle plus clair apparaît autour du glyphe.

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
| `pLight` | Picture | ampoule de la tuile Lumières (65 éteinte / 66 allumée) |
| `tRoom` | Text | pièce par défaut |
| `tLights` | Text | ex. « 3 allumées » |
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
| `hSleep` | Slider | délai de veille |
| `tSsid`, `tIp`, `tHa` | Text | diagnostic réseau/HA |
| `bOta`, `bTft`, `bReboot` | Button | actions (hotspots) |
| `tVer` | Text | version firmware |

La saisie de l'URL/token HA reste sur le **portail web** (`http://<ip>/`),
pas sur cette page — pas de clavier tactile pour un jeton de 180 caractères.

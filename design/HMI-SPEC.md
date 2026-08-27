# Spécification HMI — nspanel-ha

Référence pour le montage du `.HMI` dans Nextion Editor. Source de vérité
couplée à `main/bsp/nx_objects.def` et `main/bsp/nx_pages.def` — toute
modification ici doit se refléter dans ces deux fichiers (et réciproquement).

## Général

- Écran : Nextion Discovery NX4832F035, résistif.
- **Orientation : à trancher.** Cette spec a été écrite pour 320×480 *portrait* ;
  le projet Editor actuel est en 480×320 *paysage* (résolution native de la
  dalle). Les deux fonctionnent — mais toutes les positions des pages suivantes
  en dépendent, donc à figer avant de dessiner `home`.
- **Reparse mode : rien à faire dans l'Editor.** Ce n'est pas une option de
  compilation mais un état d'exécution (`recmod`) activé par le programme du
  `.tft` chargé — celui d'origine Sonoff le réaffirme en continu. C'est le
  firmware ESP32 qui l'en sort (`exit_reparse_mode()` dans `bsp/nextion.c`),
  au boot et avant chaque upload. Ne pas chercher de case à cocher.
- Tout composant écrit par le firmware doit avoir `vscope=global`, sinon
  impossible de le pré-remplir avant d'y naviguer.
- Polices `.zi` (chasse fixe) : `f0` 16 px, `f1` 24 px, `f2` 48 px.
  **Restreindre le jeu de caractères à la génération** (ASCII, ou Latin-1 si on
  veut les accents) : avec un jeu large, une seule police 16 px a produit
  2,03 Mo sur un `.tft` de 2,43 Mo — soit 84 % du fichier, et plusieurs minutes
  par upload à 115200 bauds au lieu de quelques secondes. Le panneau Output de
  l'Editor donne `Total size of font:` — c'est le chiffre à surveiller.
- Icônes : pas de police d'icônes — glyphes Material exportés en PNG, ajoutés
  en ressources Picture, affichés par `pic`/`picq`.
- Palette RGB565 dérivée de `ui_common.h` du panneau P4 :
  fond `0x0841` (≈`COL_BG` 0x0d1117), lumières `0xF585` (`COL_LIGHTS`
  0xf5b133), chauffage `0xF337` (`COL_HEAT` 0xf2683c), accueil `0x39F8`
  (`COL_HOME` 0x3b82f6).

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
| `tRoom` | Text | pièce par défaut |
| `tLights` | Text | ex. « 3 allumées » |
| `tTemp` | Text (f1) | température courante (chauffage pièce) |
| `tSet` | Text | consigne |
| `pWx` | Picture | icône météo courante |
| `tWxT` | Text | température météo |
| `tWxC` | Text | condition météo (libellé court) |
| `mLight`, `mHeat`, `mWx`, `mCfg` | Hotspot | navigation (non sondés — absence silencieuse) |

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

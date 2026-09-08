# Prompt ChatGPT — jeu d'images Nextion

> **Le jeu d'images existe déjà** : il est généré en local par
> `design/images/gen_images.py` (voir « Ressources Picture » dans `HMI-SPEC.md`).
> Ce prompt ne sert plus qu'à faire *redessiner* le jeu par ChatGPT — par exemple
> à partir d'une capture d'écran du haPanel, pour un rendu plus proche de
> l'original que les tracés géométriques du script.

Mode d'emploi : ouvrir ChatGPT (modèle avec exécution de code), **joindre la capture
d'écran du haPanel**, coller le prompt ci-dessous tel quel, et récupérer le zip + la
table. La check-list de contrôle est en fin de fichier.

---

## Prompt à copier

Tu es graphiste d'interface embarquée. Tu produis un jeu complet d'images pour un écran
**Nextion Discovery NX4832F035** (3,5", **480×320 paysage**, dalle résistive, dont les
**25 px de droite sont masqués par le cadre** : les images font bien 480 px de large, mais
aucun contenu utile ne doit dépasser `x = 447`), qui sert
d'interface murale Home Assistant. La capture jointe est l'interface existante d'un autre
panneau du même foyer (CrowPanel P4, LVGL) : **c'est la référence de design** — même
langage visuel, mêmes cartes, même hiérarchie typographique. Tu la suis pour la
composition et l'esprit, pas pour les couleurs : la palette ci-dessous fait foi.

### Contraintes techniques, non négociables

- La série Discovery **ne gère pas la transparence**. Chaque PNG doit être **24 bits
  RGB, sans canal alpha**, aplati sur la couleur de fond de l'endroit où il sera posé
  (`#0d1117` par défaut, `#161c24` si l'icône est posée sur une carte — je précise
  lequel dans l'inventaire).
- Le rendu final est quantifié en **RGB565** (5 bits de rouge, 6 de vert, 5 de bleu).
  Donc : aplats francs, **aucun dégradé subtil**, aucune ombre portée douce, aucun
  halo. Un dégradé qui « bande » est pire que pas de dégradé.
- **Traits ≥ 2 px** et formes pleines : à 32 px sur une dalle résistive derrière une
  vitre, un trait de 1 px disparaît.
- Aucun texte dans les images **sauf** les libellés fixes explicitement demandés dans
  les fonds de page (les valeurs dynamiques sont écrites par le firmware par-dessus).
- Budget total du jeu : **< 800 Ko**. L'upload du `.tft` vers le panneau se fait à
  ~11 Ko/s ; chaque 100 Ko coûte 10 s de transfert à chaque itération de design.

### Palette imposée

| Rôle | Hex 24 bits | RGB565 |
|---|---|---|
| Fond général | `#0d1117` | `0x0841` |
| Carte | `#161c24` | — |
| Carte alternative | `#1b2230` | — |
| Texte | `#eef2f6` | — |
| Texte atténué | `#8a93a2` | — |
| Texte effacé | `#5a636e` | — |
| Accent lumières | `#f5b133` | `0xF585` |
| Accent chauffage | `#f2683c` | `0xF337` |
| Accent accueil / navigation | `#3b82f6` | `0x39F8` |
| Vert (ok / éco) | `#34d399` | — |
| Rouge (alerte) | `#ef4444` | — |

N'introduis aucune autre couleur. Si la capture en montre d'autres, ramène-les à la
plus proche de cette table.

### Méthode de production imposée

Tu **ne génères pas** ces images avec un modèle d'image. Tu procèdes ainsi :

1. Tu écris les **SVG** (tracés Material Symbols pour les icônes, formes géométriques
   pour les fonds de page), en vectoriel propre, viewBox carré pour les icônes.
2. Tu écris un **script Python** (`cairosvg` + `Pillow`) qui, pour chaque entrée d'une
   table de spécification codée en dur dans le script :
   - rastérise le SVG **exactement** à la taille demandée (pas de redimensionnement
     après coup) ;
   - compose le résultat sur un calque uni de la couleur de fond indiquée ;
   - convertit en `RGB` (`Image.convert("RGB")`, jamais `RGBA`) et écrit le PNG ;
   - accumule une ligne de manifeste.
3. Tu **exécutes** ce script, tu écris `manifest.csv` avec les colonnes
   `fichier, pic_id, page, objet, role, format, largeur, hauteur, octets`, et tu
   empaquettes tout dans un zip que tu me donnes en téléchargement.

Interdits : PNG 32 bits, redimensionnement approximatif, image générée par diffusion,
fichier absent du manifeste.

### Les `pic` id — lis ce paragraphe avant de nommer quoi que ce soit

Sur Nextion, l'id d'une image **n'est pas un champ que l'on choisit** : c'est le rang de
la ressource dans la liste Picture de l'éditeur, attribué dans l'ordre d'ajout. Le
firmware, lui, écrit cet entier (`home.pWx.pic=13`) et calcule souvent
`base + index_de_condition`. Donc :

- Tu produis les fichiers **préfixés de leur id sur deux chiffres** (`13_wx64_rainy.png`),
  pour que l'ordre alphabétique du zip soit exactement l'ordre d'ajout dans l'éditeur.
- Les blocs doivent rester **contigus et dans l'ordre exact** du tableau ci-dessous.
- Les trous entre blocs (id 6–9, 19) sont **volontaires** : ils laissent de la marge.
  Génère pour chacun une image de garde **1×1 px** noire nommée `06_reserved.png` etc.,
  sinon tout le bloc suivant se décale d'un cran à l'import et le firmware affiche la
  mauvaise icône.

### Inventaire à produire

**Bloc fonds de page — id 0 à 5, 480×320, fond `#0d1117`**

| id | fichier | page | contenu |
|---|---|---|---|
| 0 | `00_bg_boot.png` | boot | fond nu + un bandeau de titre discret en haut ; grande zone de texte vide au centre (statut WiFi/IP/HA sur 3–4 lignes) et emplacement d'une barre de progression en bas (rail creux `#1b2230`, la barre elle-même est un composant Nextion, ne la dessine pas) |
| 1 | `01_bg_home.png` | home | reprend la composition de la capture : grande zone horloge en haut à gauche (vide), 4 tuiles carrées en bas (cartes `#161c24`, coins arrondis 8 px) pour lumières / chauffage / météo / réglages, avec sous chaque tuile un liseré de 3 px de sa couleur d'accent ; deux zones de valeur vides à droite (température, météo) |
| 2 | `02_bg_lights.png` | lights | en-tête (nom de pièce, vide) encadré de deux chevrons ; 8 lignes de carte `#161c24` hauteur ~28 px, chacune avec à gauche la zone du nom et à droite l'emplacement d'un interrupteur et d'un rail de curseur (rail seul, `#1b2230`) ; bouton « tout éteindre » en bas |
| 3 | `03_bg_climate.png` | climate | anneau/cadran creux au centre (arc épais `#1b2230`, non rempli) laissant vide l'intérieur pour la température ; emplacements ± de part et d'autre ; bande de 3 préréglages en bas |
| 4 | `04_bg_weather.png` | weather | carte conditions actuelles en haut (icône 64 px à gauche, valeurs à droite, tout vide) ; ligne de 3 métriques (humidité / vent / pression) avec leurs libellés fixes en `#8a93a2` ; bande de 4 mini-cartes de prévision en bas, chacune avec la place d'une icône 40 px |
| 5 | `05_bg_cfg.png` | cfg | liste de réglages : deux lignes avec rail de curseur (luminosité, veille), trois lignes de diagnostic à libellés fixes `SSID`, `IP`, `HA` en `#8a93a2`, trois boutons d'action en bas, ligne de version en pied |

Dans **tous** les fonds : les zones où le firmware écrira du texte restent des **aplats
unis** (pas de cadre serré, pas de texte de remplissage) — les libellés fixes autorisés
sont uniquement ceux nommés ci-dessus.

**Garde — id 6 à 9** : `06_reserved.png` … `09_reserved.png`, 1×1 px, `#000000`.

**Bloc météo grand format — id 10 à 18, 64×64, fond `#161c24`, icône en `#eef2f6`**

Ordre **impératif** (il correspond à l'ordre des conditions Home Assistant côté firmware) :

| id | fichier | condition HA | glyphe Material |
|---|---|---|---|
| 10 | `10_wx64_sunny.png` | `sunny` | `wb_sunny` |
| 11 | `11_wx64_clear-night.png` | `clear-night` | `brightness_3` |
| 12 | `12_wx64_partlycloudy.png` | `partlycloudy` | `wb_cloudy` |
| 13 | `13_wx64_cloudy.png` | `cloudy` | `cloud` |
| 14 | `14_wx64_rainy.png` | `rainy`, `pouring`, `hail`, `snowy-rainy` | `grain` |
| 15 | `15_wx64_snowy.png` | `snowy` | `ac_unit` |
| 16 | `16_wx64_lightning.png` | `lightning`, `lightning-rainy` | `bolt` |
| 17 | `17_wx64_windy.png` | `windy`, `windy-variant` | `waves` |
| 18 | `18_wx64_fog.png` | `fog`, `exceptional`, inconnu | `foggy` |

**Garde — id 19** : `19_reserved.png`, 1×1 px.

**Bloc météo petit format — id 20 à 28, 40×40, fond `#161c24`, icône en `#eef2f6`**

Exactement les mêmes 9 conditions, **dans le même ordre**, nommées `20_wx40_sunny.png`
… `28_wx40_fog.png`. Ce sont des tracés redessinés pour 40 px (simplifiés si besoin),
pas les 64 px rétrécis.

**Bloc icônes d'interface — id 30 à 55, 32×32, fond `#161c24`**

Chaque icône en **deux variantes consécutives** : d'abord inactive (`#8a93a2`), puis
active (couleur indiquée). Nommage `NN_<nom>_off.png` / `NN_<nom>_on.png`.

| id off / on | nom | glyphe | couleur active |
|---|---|---|---|
| 30 / 31 | `light` | `lightbulb` | `#f5b133` |
| 32 / 33 | `heat` | `device_thermostat` | `#f2683c` |
| 34 / 35 | `weather` | `cloud` | `#3b82f6` |
| 36 / 37 | `cfg` | `settings` | `#3b82f6` |
| 38 / 39 | `plus` | `add` | `#f2683c` |
| 40 / 41 | `minus` | `remove` | `#f2683c` |
| 42 / 43 | `boost` | `local_fire_department` | `#f2683c` |
| 44 / 45 | `eco` | `eco` | `#34d399` |
| 46 / 47 | `frost` | `ac_unit` | `#3b82f6` |
| 48 / 49 | `power` | `power_settings_new` | `#ef4444` |
| 50 / 51 | `wifi` | `wifi` | `#34d399` |
| 52 / 53 | `prev` | `chevron_left` | `#eef2f6` |
| 54 / 55 | `next` | `chevron_right` | `#eef2f6` |

Soit **56 fichiers** (dont 5 gardes 1×1).

### Format de ta réponse

1. D'abord la **table du manifeste en Markdown** (toutes les lignes, colonnes du
   `manifest.csv`), avec le total en octets en dernière ligne.
2. Ensuite le **lien de téléchargement du zip** (PNG + `manifest.csv`).
3. En annexe, le **script Python** complet et les SVG, pour que je puisse régénérer ou
   changer une couleur sans repasser par toi.

Si une contrainte t'oblige à dévier (glyphe Material introuvable, budget dépassé),
dis-le en une ligne avant la table, et propose le remplacement — ne dévie pas en silence.

---

## Check-list de contrôle du livrable

À passer sur le zip décompressé avant d'importer quoi que ce soit dans l'éditeur :

```bash
# 1. Aucun PNG ne doit être RGBA (la série Discovery ignore l'alpha)
file *.png | grep -v "8-bit/color RGB" || echo "OK : tout est en RGB 24 bits"

# 2. Dimensions exactes
python3 - <<'PY'
from PIL import Image
import glob, re
att = {"bg": (480, 320), "wx64": (64, 64), "wx40": (40, 40)}
for f in sorted(glob.glob("*.png")):
    im = Image.open(f)
    kind = next((k for k in att if k in f), None)
    exp = att.get(kind, (32, 32) if "reserved" not in f else (1, 1))
    print(("OK  " if im.size == exp else "FAUX"), f, im.size, im.mode)
PY

# 3. Ordre d'ajout = ordre alphabétique : vérifier qu'aucun id ne manque de 00 à 55
ls *.png | cut -c1-2 | sort -n | uniq | tr '\n' ' '
```

Puis, une fois les images ajoutées dans l'ordre alphabétique à la liste Picture de
Nextion Editor, `.tft` compilé et uploadé (cf. « Cycle d'itération » dans
`HMI-SPEC.md`), la seule preuve que l'allocation d'id tient :

```bash
curl -X POST --data-urlencode "cmd=home.pWx.pic=14" http://<panneau>/nx   # doit afficher la pluie
curl -X POST --data-urlencode "cmd=get home.pWx.pic" http://<panneau>/nx  # doit renvoyer 14
```

Si l'icône affichée n'est pas celle attendue, c'est qu'une ressource a été ajoutée hors
ordre : tout le bloc est décalé, il faut réimporter, pas corriger le firmware.

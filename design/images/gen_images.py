#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Génère le jeu de ressources Picture du .tft — cf. design/HMI-SPEC.md.

    python3 design/images/gen_images.py [dossier_sortie]

Sortie : PNG **24 bits sans alpha** (la série Discovery ignore la transparence)
+ manifest.csv. Chaque fichier est préfixé de son `pic` id : l'id d'une
ressource Nextion est son rang d'insertion dans la liste Picture de l'Editor,
donc **ajouter les fichiers dans l'ordre alphabétique**, gardes 1×1 comprises.

Deux partis pris hérités du panneau P4 (cf. la photo de référence) :

  * `home` n'a pas de cartes — fond noir, sections signalées par une icône et
    un libellé de la couleur du domaine. Seule la météo est une carte, en
    dégradé bleu.
  * Faute de canal alpha, une icône posée sur un dégradé est aplatie sur le
    **morceau exact de fond** qu'elle recouvre. Elle est donc liée à sa
    position : déplacer le composant dans l'Editor oblige à régénérer. Les
    coordonnées font foi ici, colonne `role` du manifeste.

Tout est ramené à la grille RGB565 avec diffusion d'erreur : les aplats de la
palette y tombent juste (erreur nulle, aucun bruit) et les dégradés sortent
tramés, sans escalier — c'est ce que fait `ui_grad_v()` sur le P4.
"""
import csv
import io
import os
import re
import sys

import cairosvg
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icons import ICONS  # noqa: E402

W, H = 480, 320                       # 480×320 paysage (résolution native)

# La dalle est physiquement masquée sur ~5 px à DROITE par le cadre du boîtier :
# ces colonnes existent dans l'image et dans les coordonnées Nextion, mais ne
# sont jamais visibles. Les images gardent donc leur taille native (W), et c'est
# la MISE EN PAGE qui se replie sur `WU` — sinon tout ce qui est calé à droite
# (icône cfg, bord des cartes, flèches) disparaît sous le cadre.
# À reporter dans l'Editor : voir design/EDITOR-HOME.md et design/EDITOR-CFG.md.
MASK_R = 25
WU = W - MASK_R                       # 455 px réellement visibles
M = 8                                 # marge de page, identique à gauche et à droite


# --- palette --------------------------------------------------------------
# Source de vérité : `ui_common.h` du panneau P4, lu à la génération plutôt que
# recopié — une teinte ne doit exister qu'à un seul endroit pour les deux
# panneaux. Le dépôt voisin n'est pas une dépendance dure : sans lui, on
# retombe sur la copie ci-dessous (à resynchroniser si le P4 bouge).

HA_PANEL_UI_COMMON = os.environ.get(
    "HA_PANEL_UI_COMMON",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "..", "ha-panel", "main", "ui", "ui_common.h"))

# Poids de la teinte d'accent en haut d'une carte, tel que le P4 le définit
# (`UI_CARD_ACCENT_W`, sur 255) : le dégradé va de mix(accent, COL_CARD, w) en
# haut à COL_CARD en bas. Lu à la génération comme la palette — la recette ne
# doit pas diverger entre les deux panneaux.
ACCENT_W_FALLBACK = 140

PALETTE_FALLBACK = {
    "COL_BG": 0x0d1117, "COL_SIDEBAR": 0x11161d, "COL_CARD": 0x161c24,
    "COL_CARD2": 0x1b2230, "COL_TXT": 0xeef2f6, "COL_TXT_DIM": 0x8a93a2,
    "COL_TXT_FAINT": 0x5a636e, "COL_LIGHTS": 0xf5b133, "COL_HEAT": 0xf2683c,
    "COL_HEAT_SOFT": 0xf9a07a, "COL_HOME": 0x3b82f6, "COL_GREEN": 0x34d399,
    "COL_RED": 0xef4444, "COL_NAVY": 0x1e3a8a, "COL_NAVY_LT": 0x6d7fb3,
    "SKY_DAY_T": 0x2563eb, "SKY_DAY_B": 0x0e2a5e,
}


def load_palette():
    """Couleurs de `ui_common.h`, ou la copie de secours si le P4 est absent."""
    try:
        src = io.open(HA_PANEL_UI_COMMON, encoding="utf-8").read()
    except OSError:
        print("ui_common.h introuvable (%s) — palette de secours"
              % HA_PANEL_UI_COMMON, file=sys.stderr)
        return dict(PALETTE_FALLBACK), "copie locale"
    found = dict(re.findall(r"#define\s+((?:COL|SKY)_\w+)\s+0x([0-9a-fA-F]{6})", src))
    pal = {k: int(v, 16) for k, v in found.items()}
    missing = [k for k in PALETTE_FALLBACK if k not in pal]
    if missing:
        raise SystemExit("couleurs absentes de %s : %s"
                         % (HA_PANEL_UI_COMMON, ", ".join(missing)))
    m = re.search(r"#define\s+UI_CARD_ACCENT_W\s+(\d+)", src)
    pal["_ACCENT_W"] = int(m.group(1)) if m else ACCENT_W_FALLBACK
    return pal, HA_PANEL_UI_COMMON


def snap565(rgb24):
    """Couleur ramenée à la valeur exacte que l'écran affichera en RGB565.

    Arrondi au plus proche, pas troncature : `>>3` décale toute la palette vers
    le sombre (jusqu'à 4 %), ce qui se voit sur les gris. Une couleur déjà
    exacte traverse `dither565()` sans erreur, donc sans bruit sur les aplats."""
    out = []
    for shift, bits in ((16, 5), (8, 6), (0, 5)):
        v = (rgb24 >> shift) & 0xff
        top = (1 << bits) - 1
        out.append(int(round(v * top / 255.0)) * 255 // top)
    return "#%02x%02x%02x" % tuple(out)


PAL, PAL_SRC = load_palette()
_c = lambda name: snap565(PAL[name])
ACCENT_W = PAL.get("_ACCENT_W", ACCENT_W_FALLBACK)

BG        = _c("COL_BG")
CARD      = _c("COL_CARD")
CARD2     = _c("COL_CARD2")
TXT       = _c("COL_TXT")
DIM       = _c("COL_TXT_DIM")
FAINT     = _c("COL_TXT_FAINT")
LIGHTS    = _c("COL_LIGHTS")
HEAT      = _c("COL_HEAT")
HEAT_SOFT = _c("COL_HEAT_SOFT")       # anneau « température actuelle »
HOME      = _c("COL_HOME")
GREEN     = _c("COL_GREEN")
RED       = _c("COL_RED")
NAVY      = _c("COL_NAVY")            # séparateurs sur le dégradé
SKY_T     = _c("SKY_DAY_T")           # dégradé météo, haut
SKY_B     = _c("SKY_DAY_B")           # dégradé météo, bas

F_REG  = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
F_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Conditions météo : l'ordre EST le contrat avec le firmware (base + index).
WX = ["sunny", "clear-night", "partlycloudy", "cloudy", "rainy",
      "snowy", "lightning", "windy", "fog"]

# Icônes d'interface : (nom, glyphe, couleur active) — deux fichiers chacune,
# inactive (DIM) puis active.
UI = [
    ("light",   "light",   LIGHTS),
    ("heat",    "heat",    HEAT),
    ("weather", "weather", HOME),
    ("cfg",     "cfg",     HOME),
    ("plus",    "plus",    HEAT),
    ("minus",   "minus",   HEAT),
    ("boost",   "boost",   HEAT),
    ("eco",     "eco",     GREEN),
    ("frost",   "frost",   HOME),
    ("power",   "power",   RED),
    ("wifi",    "wifi",    GREEN),
    ("prev",    "prev",    TXT),
    ("next",    "next",    TXT),
]

# Emplacements des Pictures posées sur un dégradé — à reporter tels quels dans
# l'Editor, l'aplatissement en dépend.
SLOT_HOME_WX = (26, 232)              # home.pWx, 64×64
SLOT_WX_NOW  = (24, 54)               # weather.pNow, 64×64
SLOT_WX_DAY  = (0, 236)               # weather.pD0..pD3, 40×40 (y seul : même bande)


# --- rendu ----------------------------------------------------------------

def svg_rgba(glyph, size, fg, bg):
    body = ICONS[glyph].replace("{c}", fg).replace("{bg}", bg)
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
           'width="%d" height="%d">%s</svg>' % (size, size, body))
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size,
                           output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def icon_on(bg_color, glyph, size, fg):
    """Icône aplatie sur un aplat."""
    out = Image.new("RGBA", (size, size), bg_color)
    out.alpha_composite(svg_rgba(glyph, size, fg, bg_color))
    return out.convert("RGB")


def icon_on_patch(page_img, xy, glyph, size, fg):
    """Icône aplatie sur le morceau de fond qu'elle recouvrira réellement."""
    x, y = xy
    patch = page_img.crop((x, y, x + size, y + size)).convert("RGBA")
    patch.alpha_composite(svg_rgba(glyph, size, fg, BG))
    return patch.convert("RGB")


def stamp(img, xy, glyph, size, fg):
    """Grave une icône dans un fond (elle ne changera jamais)."""
    img.paste(icon_on_patch(img, xy, glyph, size, fg), xy)


def dither565(img):
    """Quantifie sur la grille RGB565 par diffusion d'erreur (Floyd–Steinberg).

    Les couleurs de la palette sont déjà exactes (snap565) : erreur nulle, donc
    aucun bruit sur les aplats. Seuls les dégradés et l'anticrénelage sont
    tramés — l'écran affiche alors exactement ces pixels, sans requantifier."""
    w, h = img.size
    raw = img.convert("RGB").tobytes()
    buf = [[float(raw[i]), float(raw[i + 1]), float(raw[i + 2])]
           for i in range(0, len(raw), 3)]             # ligne à plat, w*h
    step = (255.0 / 31, 255.0 / 63, 255.0 / 31)
    top = (31, 63, 31)
    for y in range(h):
        base = y * w
        for x in range(w):
            i = base + x
            old = buf[i]
            new = [0.0, 0.0, 0.0]
            err = [0.0, 0.0, 0.0]
            for c in range(3):
                q = round(old[c] / step[c])
                q = 0 if q < 0 else (top[c] if q > top[c] else q)
                new[c] = q * step[c]
                err[c] = old[c] - new[c]
            buf[i] = new
            if x + 1 < w:
                n = buf[i + 1]
                for c in range(3):
                    n[c] += err[c] * 0.4375
            if y + 1 < h:
                if x:
                    n = buf[i + w - 1]
                    for c in range(3):
                        n[c] += err[c] * 0.1875
                n = buf[i + w]
                for c in range(3):
                    n[c] += err[c] * 0.3125
                if x + 1 < w:
                    n = buf[i + w + 1]
                    for c in range(3):
                        n[c] += err[c] * 0.0625
    flat = bytearray(w * h * 3)
    for i, p in enumerate(buf):
        for c in range(3):
            flat[i * 3 + c] = min(255, max(0, int(round(p[c]))))
    return Image.frombytes("RGB", (w, h), bytes(flat))


# --- primitives de fond ---------------------------------------------------

def card(d, box, fill=CARD, r=8):
    d.rounded_rectangle(box, radius=r, fill=fill)


def rail(d, x0, y, x1, h=8):
    """Rail creux d'un slider — le curseur est un composant Nextion."""
    d.rounded_rectangle((x0, y, x1, y + h), radius=h // 2, fill=CARD2)


def label(d, xy, text, color=DIM, size=13, anchor="la", bold=False):
    d.text(xy, text, font=ImageFont.truetype(F_BOLD if bold else F_REG, size),
           fill=color, anchor=anchor)


def _rgb(hexstr):
    return [int(hexstr[i:i + 2], 16) for i in (1, 3, 5)]


def mix(a, b, wa):
    """Mélange "#rrggbb" : wa/255 de `a` + (255-wa)/255 de `b`.

    Reprend ui_mix() du P4 (ha-panel/main/ui/ui_common.c) — même formule, pour
    que les teintes dérivées (têtes de carte) soient identiques sur les deux
    panneaux. Le résultat repasse par snap565 : une teinte calculée doit tomber
    sur la grille comme les couleurs de la palette, sinon elle ressort tramée
    sur un aplat."""
    ra, rb = _rgb(a), _rgb(b)
    v = [(ra[c] * wa + rb[c] * (255 - wa)) // 255 for c in range(3)]
    return snap565((v[0] << 16) | (v[1] << 8) | v[2])


def grad(img, box, top, bot, r=14):
    """Dégradé vertical `top`→`bot`, coins arrondis, collé dans `img`."""
    x0, y0, x1, y1 = box
    t, b = _rgb(top), _rgb(bot)
    gw, gh = x1 - x0, y1 - y0
    g = Image.new("RGB", (gw, gh))
    gd = ImageDraw.Draw(g)
    for y in range(gh):
        k = y / float(gh - 1)
        gd.line((0, y, gw, y),
                fill=tuple(int(round(t[c] + (b[c] - t[c]) * k)) for c in range(3)))
    m = Image.new("L", (gw, gh), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, gw - 1, gh - 1), radius=r, fill=255)
    img.paste(g, (x0, y0), m)


def sky(img, box, r=14):
    """Carte (ou page) en dégradé bleu vertical — page/carte Météo."""
    grad(img, box, SKY_T, SKY_B, r)


def accent_card(img, box, accent, r=12):
    """Carte teintée : accent en tête fondu vers COL_CARD, recette du P4.

    C'est `card()` de ha-panel/main/ui/ui_common.c:95 — mix(accent, COL_CARD,
    UI_CARD_ACCENT_W) en haut, COL_CARD en bas. Ce qui donne aux tuiles
    Lumières (jaune) et Chauffage (orange) leur dégradé, au lieu de l'aplat
    COL_CARD qu'elles avaient ici."""
    grad(img, box, mix(accent, CARD, ACCENT_W), CARD, r)


def cols(n, margin=16, gap=10, width=None):
    """`n` colonnes de largeur égale dans la zone VISIBLE (WU), pas dans W.

    Renvoie [(x, largeur)] — utilisé par les rangées de trois cartes (actions
    de cfg, préréglages de climate, métriques de weather) pour qu'elles restent
    centrées sur ce que l'œil voit, cadre compris."""
    total = (width if width is not None else WU) - 2 * margin
    w = (total - gap * (n - 1)) // n
    return [(margin + i * (w + gap), w) for i in range(n)]


def new_bg():
    img = Image.new("RGB", (W, H), BG)
    return img, ImageDraw.Draw(img)


# --- fonds de page --------------------------------------------------------
# Les zones où le firmware écrit (tClock, tRoom, tSsid…) restent des aplats :
# seuls les libellés réellement fixes et les icônes qui ne changent jamais sont
# gravés dans l'image.

def bg_boot():
    img, d = new_bg()
    stamp(img, (14, 12), "wifi", 20, DIM)
    label(d, (42, 22), "nspanel-ha", TXT, 15, "lm")
    label(d, (WU - 16, 22), "HOME ASSISTANT", FAINT, 11, "rm")
    d.line((14, 40, WU - 14, 40), fill=CARD2, width=1)
    rail(d, 24, 272, WU - 24, 14)                 # emplacement de jProg
    return img


# Tuiles de `home`, calées sur la zone visible : deux tuiles de largeur égale
# entre M et WU-M. Ces valeurs sont le contrat avec l'Editor (positions des
# objets posés dessus) ET avec le firmware (centre des anneaux, HOME_ARC_CX
# dans main/ui/ui.c) — voir design/EDITOR-HOME.md.
TILE_Y0, TILE_Y1 = 52, 190
_tw = ((WU - 2 * M) - 17) // 2                    # 17 px de gouttière
TILE_L = (M, TILE_Y0, M + _tw, TILE_Y1)           # Lumières
TILE_R = (M + _tw + 17, TILE_Y0, WU - M, TILE_Y1)  # Chauffage
CARD_WX = (M, 196, WU - M, 314)                   # carte Météo
SLOT_HOME_LIGHT = (16, 99)                        # home.pLight, 64×64


def bg_home():
    img, d = new_bg()
    stamp(img, (WU - 14 - 22, 12), "cfg", 22, FAINT)   # mCfg
    # Tuile Lumières — dégradé jaune (accent COL_LIGHTS) + en-tête gravés ;
    # pLight (bilan on/off, 65/66) et les valeurs (tRoom, tLights) sont des
    # objets Nextion posés dessus.
    accent_card(img, TILE_L, LIGHTS)
    stamp(img, (16, 60), "light", 20, LIGHTS)
    label(d, (42, 70), "Lumières", LIGHTS, 14, "lm")
    # Tuile Chauffage — dégradé orange (accent COL_HEAT) ; les anneaux sont
    # dessinés à l'exécution par le firmware (nx_draw_arc, main/ui/ui.c), pas
    # ici : ils portent une valeur. Ne rien poser au centre (EDITOR-HOME.md).
    accent_card(img, TILE_R, HEAT)
    stamp(img, (TILE_R[0] + 8, 60), "heat", 20, HEAT)
    label(d, (TILE_R[0] + 34, 70), "Chauffage", HEAT, 14, "lm")
    # Météo : carte en dégradé bleu.
    sky(img, CARD_WX)
    stamp(img, (20, 206), "weather", 20, TXT)
    label(d, (46, 216), "Météo", TXT, 15, "lm")
    return img


def bg_lights():
    img, d = new_bg()
    stamp(img, (10, 10), "prev", 24, DIM)         # bRoomPrev
    stamp(img, (WU - 10 - 24, 10), "next", 24, DIM)   # bRoomNext
    d.line((M, 42, WU - M, 42), fill=CARD2, width=1)
    for i in range(8):
        y = 50 + i * 29
        card(d, (M, y, WU - M, y + 26), CARD, r=6)
        rail(d, 250, y + 9, 395)                  # hN<i>
        d.rounded_rectangle((WU - 18 - 44, y + 5, WU - 18, y + 21),
                            radius=8, fill=CARD2)  # bN<i>
    stamp(img, (295, 288), "power", 24, DIM)      # bAll
    label(d, (327, 300), "Tout éteindre", DIM, 13, "lm")
    return img


def bg_climate():
    img, d = new_bg()
    cx = WU // 2                                  # centré sur le VISIBLE, pas sur W
    d.arc((cx - 90, 30, cx + 90, 210), start=135, end=405, fill=HEAT_SOFT, width=12)
    d.arc((cx - 72, 48, cx + 72, 192), start=135, end=205, fill=HEAT, width=9)
    stamp(img, (40, 100), "minus", 32, HEAT)      # bDn
    stamp(img, (WU - 40 - 32, 100), "plus", 32, HEAT)   # bUp
    for (x, w), (glyph, col, txt) in zip(cols(3),
                                         (("eco", GREEN, "Absent"),
                                          ("frost", HOME, "Hors gel"),
                                          ("boost", HEAT, "Auto"))):
        card(d, (x, 268, x + w, 312), CARD, r=8)
        stamp(img, (x + 14, 282), glyph, 22, col)
        label(d, (x + 44, 291), txt, DIM, 13, "lm")
    return img


def bg_weather():
    img, d = new_bg()
    # Dégradé sur toute la LARGEUR RÉELLE (W, pas WU) : les 5 px masqués n'ont
    # pas d'information, mais les laisser noirs ferait un liseré si le cadre
    # est un peu moins couvrant que prévu. Seul le contenu se replie sur WU.
    sky(img, (0, 0, W, H), r=0)
    stamp(img, (16, 12), "weather", 20, TXT)
    label(d, (42, 22), "Météo", TXT, 15, "lm")
    for (x, _w), txt in zip(cols(3), ("HUMIDITÉ", "VENT", "PRESSION")):
        label(d, (x + 12, 156), txt, DIM, 11)
    d.line((M, 146, WU - M, 146), fill=NAVY, width=1)
    d.line((M, 190, WU - M, 190), fill=NAVY, width=1)
    return img


def bg_cfg():
    """cfg — retour, carte pièce (prev/next), luminosité/veille (curseurs
    natifs Nextion, pas de rail dessiné : bco du Slider en tient lieu),
    diagnostics, actions, version. Voir design/EDITOR-CFG.md pour la géométrie
    exacte des composants posés par-dessus (bRet, tRoom, hDim, hSleep…)."""
    img, d = new_bg()
    stamp(img, (14, 17), "prev", 24, TXT)                      # bRet
    card(d, (56, 6, WU - M, 52), CARD, r=8)                    # tRoom
    label(d, (66, 16), "PIÈCE", DIM, 11)
    stamp(img, (WU - 66 - 24, 16), "prev", 24, TXT)            # bRoomPrev
    stamp(img, (WU - 26 - 24, 16), "next", 24, TXT)            # bRoomNext
    label(d, (16, 66), "LUMINOSITÉ", DIM, 12)                  # hDim
    label(d, (16, 112), "VEILLE", DIM, 12)                     # hSleep
    for i, txt in enumerate(("SSID", "IP", "HA")):             # tSsid, tIp, tHa
        y = 156 + i * 31
        card(d, (M, y, WU - M, y + 28), CARD, r=6)
        label(d, (18, y + 14), txt, DIM, 12, "lm")
    for (x, w), (glyph, col, txt) in zip(cols(3),
                                         (("cfg", HOME, "OTA"),
                                          ("weather", HOME, "TFT"),
                                          ("power", RED, "Reboot"))):
        card(d, (x, 252, x + w, 292), CARD, r=8)
        stamp(img, (x + 14, 262), glyph, 22, col)
        label(d, (x + 44, 272), txt, DIM, 13, "lm")
    card(d, (M, 296, WU - M, 316), CARD2, r=6)                 # tVer
    return img


BACKGROUNDS = [
    ("bg_boot",    "boot",    bg_boot,    "en-tête + rail de jProg"),
    ("bg_home",    "home",    bg_home,    "sections Lumières/Chauffage, anneaux, carte météo dégradée"),
    ("bg_lights",  "lights",  bg_lights,  "en-tête pièce + 8 lignes de lumière"),
    ("bg_climate", "climate", bg_climate, "anneaux, ±, 3 préréglages"),
    ("bg_weather", "weather", bg_weather, "page en dégradé, 3 métriques, 4 jours"),
    ("bg_cfg",     "cfg",     bg_cfg,     "2 curseurs, 3 diagnostics, 3 actions"),
]

GUARD = "réservé — ne pas supprimer, décalerait tous les id suivants"


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    rows, state = [], {"pid": 0}

    def emit(img, name, page, obj, role):
        pid = state["pid"]
        state["pid"] += 1
        path = os.path.join(outdir, "%02d_%s.png" % (pid, name))
        img.save(path, optimize=True)
        rows.append(dict(fichier=os.path.basename(path), pic_id=pid, page=page,
                         objet=obj, role=role, format="PNG RGB 24 bits",
                         largeur=img.width, hauteur=img.height,
                         octets=os.path.getsize(path)))

    def guard():
        emit(Image.new("RGB", (1, 1), "#000000"), "reserved", "-", "-", GUARD)

    pages = {name: dither565(fn()) for name, _, fn, _ in BACKGROUNDS}

    for name, page, _, role in BACKGROUNDS:                     # 0–5
        emit(pages[name], name, page, "(fond de page)", role)
    while state["pid"] < 10:                                    # 6–9
        guard()

    for c in WX:                                                # 10–18
        emit(icon_on_patch(pages["bg_weather"], SLOT_WX_NOW, c, 64, TXT),
             "wx64_" + c, "weather", "pNow",
             "condition « %s » — aplatie sur le dégradé en %s" % (c, SLOT_WX_NOW))
    guard()                                                     # 19

    for c in WX:                                                # 20–28
        emit(icon_on_patch(pages["bg_weather"], (8, SLOT_WX_DAY[1]), c, 40, TXT),
             "wx40_" + c, "weather", "pD0..pD3",
             "prévision « %s » — aplatie sur le dégradé en y=%d, x libre"
             % (c, SLOT_WX_DAY[1]))
    guard()                                                     # 29

    for name, glyph, active in UI:                              # 30–55
        emit(icon_on(BG, glyph, 32, DIM), name + "_off", "-", "-",
             "icône %s, inactive — fond %s" % (name, BG))
        emit(icon_on(BG, glyph, 32, active), name + "_on", "-", "-",
             "icône %s, active (%s)" % (name, active))

    for c in WX:                                                # 56–64
        emit(icon_on_patch(pages["bg_home"], SLOT_HOME_WX, c, 64, TXT),
             "home_wx64_" + c, "home", "pWx",
             "condition « %s » — aplatie sur la carte météo en %s"
             % (c, SLOT_HOME_WX))

    for suffix, col in (("off", DIM), ("on", LIGHTS)):          # 65–66
        # Aplatie sur le MORCEAU EXACT de la tuile Lumières qu'elle recouvre :
        # depuis que cette tuile est en dégradé (accent_card), un aplat CARD
        # laisserait un rectangle plus sombre autour de l'ampoule. pLight
        # devient donc liée à sa position, comme pWx — la déplacer dans
        # l'Editor oblige à corriger SLOT_HOME_LIGHT et à régénérer.
        emit(icon_on_patch(pages["bg_home"], SLOT_HOME_LIGHT, "light", 64, col),
             "home_light64_" + suffix, "home", "pLight",
             "ampoule de la tuile Lumières, %s — aplatie sur le dégradé en %s"
             % (suffix, SLOT_HOME_LIGHT))

    with open(os.path.join(outdir, "manifest.csv"), "w", newline="",
              encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    total = sum(r["octets"] for r in rows)
    print("palette : %s" % PAL_SRC)
    print("%d fichiers, %.1f Ko au total (~%.0f s d'upload TFT à 11 Ko/s)"
          % (len(rows), total / 1024.0, total / 11264.0))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))

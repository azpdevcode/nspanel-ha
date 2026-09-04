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
import sys

import cairosvg
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from icons import ICONS  # noqa: E402

W, H = 480, 320                       # 480×320 paysage (résolution native)


def snap565(hexstr):
    """Couleur ramenée à la valeur exacte que l'écran affichera en RGB565."""
    r, g, b = (int(hexstr[i:i + 2], 16) for i in (1, 3, 5))
    r, g, b = (r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31
    return "#%02x%02x%02x" % (r, g, b)


BG      = snap565("#0d1117")
CARD    = snap565("#161c24")
CARD2   = snap565("#1b2230")
TXT     = snap565("#eef2f6")
DIM     = snap565("#8a93a2")
FAINT   = snap565("#5a636e")
LIGHTS  = snap565("#f5b133")
HEAT    = snap565("#f2683c")
HEAT_SOFT = snap565("#f9a07a")        # anneau « température actuelle »
HOME    = snap565("#3b82f6")
GREEN   = snap565("#34d399")
RED     = snap565("#ef4444")
SKY_T   = snap565("#2563eb")          # dégradé météo, haut
SKY_B   = snap565("#0e2a5e")          # dégradé météo, bas

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
    px = list(img.convert("RGB").getdata())
    buf = [[float(v) for v in p] for p in px]          # ligne à plat, w*h
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
    out = Image.new("RGB", (w, h))
    out.putdata([tuple(min(255, max(0, int(round(v)))) for v in p) for p in buf])
    return out


# --- primitives de fond ---------------------------------------------------

def card(d, box, fill=CARD, r=8):
    d.rounded_rectangle(box, radius=r, fill=fill)


def rail(d, x0, y, x1, h=8):
    """Rail creux d'un slider — le curseur est un composant Nextion."""
    d.rounded_rectangle((x0, y, x1, y + h), radius=h // 2, fill=CARD2)


def label(d, xy, text, color=DIM, size=13, anchor="la", bold=False):
    d.text(xy, text, font=ImageFont.truetype(F_BOLD if bold else F_REG, size),
           fill=color, anchor=anchor)


def sky(img, box, r=14):
    """Carte (ou page) en dégradé bleu vertical, coins arrondis."""
    x0, y0, x1, y1 = box
    t = [int(SKY_T[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(SKY_B[i:i + 2], 16) for i in (1, 3, 5)]
    gw, gh = x1 - x0, y1 - y0
    grad = Image.new("RGB", (gw, gh))
    gd = ImageDraw.Draw(grad)
    for y in range(gh):
        k = y / float(gh - 1)
        gd.line((0, y, gw, y),
                fill=tuple(int(round(t[c] + (b[c] - t[c]) * k)) for c in range(3)))
    mask = Image.new("L", (gw, gh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, gw - 1, gh - 1), radius=r, fill=255)
    img.paste(grad, (x0, y0), mask)


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
    label(d, (W - 16, 22), "HOME ASSISTANT", FAINT, 11, "rm")
    d.line((14, 40, W - 14, 40), fill=CARD2, width=1)
    rail(d, 24, 272, W - 24, 14)                  # emplacement de jProg
    return img


def bg_home():
    img, d = new_bg()
    # En-têtes de section : icône + libellé de la couleur du domaine (P4).
    stamp(img, (12, 8), "light", 22, LIGHTS)
    label(d, (40, 19), "Lumières", LIGHTS, 15, "lm")
    stamp(img, (240, 8), "heat", 22, HEAT)
    label(d, (268, 19), "Chauffage", HEAT, 15, "lm")
    stamp(img, (444, 8), "cfg", 22, FAINT)        # mCfg
    # Chauffage : anneau clair = température actuelle, arc court = consigne.
    d.arc((292, 50, 416, 174), start=135, end=405, fill=HEAT_SOFT, width=9)
    d.arc((304, 62, 404, 162), start=135, end=205, fill=HEAT, width=7)
    # Météo : seule carte de la page, en dégradé bleu.
    sky(img, (8, 196, 472, 314))
    stamp(img, (20, 206), "weather", 20, TXT)
    label(d, (46, 216), "Météo", TXT, 15, "lm")
    return img


def bg_lights():
    img, d = new_bg()
    stamp(img, (10, 10), "prev", 24, DIM)         # bRoomPrev
    stamp(img, (446, 10), "next", 24, DIM)        # bRoomNext
    d.line((8, 42, W - 8, 42), fill=CARD2, width=1)
    for i in range(8):
        y = 50 + i * 29
        card(d, (8, y, 472, y + 26), CARD, r=6)
        rail(d, 250, y + 9, 400)                  # hN<i>
        d.rounded_rectangle((418, y + 5, 462, y + 21), radius=8, fill=CARD2)  # bN<i>
    stamp(img, (300, 288), "power", 24, DIM)      # bAll
    label(d, (332, 300), "Tout éteindre", DIM, 13, "lm")
    return img


def bg_climate():
    img, d = new_bg()
    d.arc((150, 30, 330, 210), start=135, end=405, fill=HEAT_SOFT, width=12)
    d.arc((168, 48, 312, 192), start=135, end=205, fill=HEAT, width=9)
    stamp(img, (40, 100), "minus", 32, HEAT)      # bDn
    stamp(img, (408, 100), "plus", 32, HEAT)      # bUp
    for x, glyph, col, txt in ((20, "eco", GREEN, "Absent"),
                               (170, "frost", HOME, "Hors gel"),
                               (320, "boost", HEAT, "Auto")):
        card(d, (x, 268, x + 140, 312), CARD, r=8)
        stamp(img, (x + 14, 282), glyph, 22, col)
        label(d, (x + 44, 291), txt, DIM, 13, "lm")
    return img


def bg_weather():
    img, d = new_bg()
    sky(img, (0, 0, W, H), r=0)                   # page entière en dégradé
    stamp(img, (16, 12), "weather", 20, TXT)
    label(d, (42, 22), "Météo", TXT, 15, "lm")
    for x, txt in ((8, "HUMIDITÉ"), (168, "VENT"), (328, "PRESSION")):
        label(d, (x + 12, 156), txt, "#9fc0f5", 11)
    d.line((8, 146, W - 8, 146), fill="#4a74c8", width=1)
    d.line((8, 190, W - 8, 190), fill="#4a74c8", width=1)
    return img


def bg_cfg():
    img, d = new_bg()
    for i, txt in enumerate(("LUMINOSITÉ", "VEILLE")):   # hDim, hSleep
        y = 10 + i * 44
        label(d, (16, y + 4), txt, DIM, 12)
        rail(d, 16, y + 24, W - 16, 10)
    for i, txt in enumerate(("SSID", "IP", "HA")):       # tSsid, tIp, tHa
        y = 106 + i * 34
        card(d, (8, y, 472, y + 28), CARD, r=6)
        label(d, (18, y + 14), txt, DIM, 12, "lm")
    for x, glyph, col, txt in ((20, "cfg", HOME, "OTA"),
                               (170, "weather", HOME, "TFT"),
                               (320, "power", RED, "Reboot")):
        card(d, (x, 216, x + 140, 258), CARD, r=8)
        stamp(img, (x + 14, 226), glyph, 22, col)
        label(d, (x + 44, 237), txt, DIM, 13, "lm")
    card(d, (8, 288, 472, 312), CARD2, r=6)             # tVer
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
        emit(icon_on(BG, "light", 64, col), "home_light64_" + suffix, "home",
             "(Picture d'état)", "ampoule de la section Lumières, %s" % suffix)

    with open(os.path.join(outdir, "manifest.csv"), "w", newline="",
              encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    total = sum(r["octets"] for r in rows)
    print("%d fichiers, %.1f Ko au total (~%.0f s d'upload TFT à 11 Ko/s)"
          % (len(rows), total / 1024.0, total / 11264.0))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))

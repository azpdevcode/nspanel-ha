# -*- coding: utf-8 -*-
"""Tracés vectoriels des icônes, grille Material 24×24.

Monochromes : `{c}` est substitué par la couleur au rendu. Formes volontairement
épaisses (stroke ≥ 2 sur 24, soit ≥ 2,6 px à 32 px de rendu) — un trait fin
disparaît derrière la vitre d'une dalle résistive, et la quantification RGB565
n'aide pas.
"""

# Nuage réutilisé par la moitié des icônes météo.
CLOUD = ('<path d="M7.2 19.5h9.9a4.3 4.3 0 0 0 .5-8.6 6 6 0 0 0-11.4-1.2 '
         '4.6 4.6 0 0 0 1 9.8z" fill="{c}"/>')


def _rays(cx=12.0, cy=12.0, r0=7.4, r1=10.4, n=8, w=2.2):
    """n rayons régulièrement répartis, en traits épais."""
    import math
    out = []
    for i in range(n):
        a = math.radians(i * 360.0 / n)
        x0, y0 = cx + r0 * math.cos(a), cy + r0 * math.sin(a)
        x1, y1 = cx + r1 * math.cos(a), cy + r1 * math.sin(a)
        out.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="{c}" '
                   'stroke-width="%.1f" stroke-linecap="round"/>' % (x0, y0, x1, y1, w))
    return "".join(out)


ICONS = {
    # --- météo -----------------------------------------------------------
    "sunny": '<circle cx="12" cy="12" r="5.4" fill="{c}"/>' + _rays(),

    "clear-night": (
        '<path d="M20.5 15.2A9 9 0 0 1 8.8 3.5a9 9 0 1 0 11.7 11.7z" fill="{c}"/>'),

    "partlycloudy": (
        '<circle cx="8.5" cy="7.6" r="3.6" fill="{c}"/>'
        + _rays(cx=8.5, cy=7.6, r0=5.2, r1=7.4, n=8, w=1.8)
        + '<path d="M10.4 20.4h8.4a3.7 3.7 0 0 0 .4-7.4 5.2 5.2 0 0 0-9.8-1 '
          '3.9 3.9 0 0 0 1 8.4z" fill="{c}" stroke="{bg}" stroke-width="1.1"/>'),

    "cloudy": CLOUD,

    "rainy": (
        '<path d="M7.2 15.5h9.9a4.3 4.3 0 0 0 .5-8.6 6 6 0 0 0-11.4-1.2 '
        '4.6 4.6 0 0 0 1 9.8z" fill="{c}"/>'
        '<g stroke="{c}" stroke-width="2.2" stroke-linecap="round">'
        '<line x1="8.4" y1="18" x2="7.2" y2="21.4"/>'
        '<line x1="12.6" y1="18" x2="11.4" y2="21.4"/>'
        '<line x1="16.8" y1="18" x2="15.6" y2="21.4"/></g>'),

    "snowy": (
        '<path d="M7.2 15.5h9.9a4.3 4.3 0 0 0 .5-8.6 6 6 0 0 0-11.4-1.2 '
        '4.6 4.6 0 0 0 1 9.8z" fill="{c}"/>'
        '<g fill="{c}"><circle cx="8" cy="19.6" r="1.5"/>'
        '<circle cx="12" cy="20.6" r="1.5"/><circle cx="16" cy="19.6" r="1.5"/></g>'),

    "lightning": (
        '<path d="M13.6 2 5.4 13.4h5.2L9.8 22l8.6-11.8h-5.5z" fill="{c}"/>'),

    "windy": (
        '<g stroke="{c}" stroke-width="2.4" stroke-linecap="round" fill="none">'
        '<path d="M3 8.5h10a2.8 2.8 0 1 0-2.8-2.8"/>'
        '<path d="M3 13h13.5a2.8 2.8 0 1 1-2.8 2.8"/>'
        '<path d="M3 17.5h8"/></g>'),

    "fog": (
        '<path d="M7.2 13.5h9.9a4.3 4.3 0 0 0 .5-8.6 6 6 0 0 0-11.4-1.2 '
        '4.6 4.6 0 0 0 1 9.8z" fill="{c}"/>'
        '<g stroke="{c}" stroke-width="2.2" stroke-linecap="round">'
        '<line x1="4" y1="17.4" x2="20" y2="17.4"/>'
        '<line x1="6.5" y1="21" x2="17.5" y2="21"/></g>'),

    # --- interface -------------------------------------------------------
    "light": (
        '<path d="M12 2a7 7 0 0 0-4.2 12.6c.7.5 1.1 1.2 1.2 2h6c.1-.8.5-1.5 '
        '1.2-2A7 7 0 0 0 12 2z" fill="{c}"/>'
        '<rect x="9" y="18.2" width="6" height="1.9" rx=".9" fill="{c}"/>'
        '<rect x="10" y="21" width="4" height="1.7" rx=".85" fill="{c}"/>'),

    "heat": (
        '<path d="M12 2a2.6 2.6 0 0 0-2.6 2.6v9.1a4.8 4.8 0 1 0 5.2 0V4.6A2.6 '
        '2.6 0 0 0 12 2z" fill="none" stroke="{c}" stroke-width="2"/>'
        '<circle cx="12" cy="17.8" r="2.6" fill="{c}"/>'
        '<rect x="11" y="7.5" width="2" height="8" fill="{c}"/>'),

    "weather": CLOUD,

    "cfg": (
        '<circle cx="12" cy="12" r="3.4" fill="none" stroke="{c}" stroke-width="2.4"/>'
        + "".join(
            '<g transform="rotate(%d 12 12)"><rect x="10.6" y="1.6" width="2.8" '
            'height="4.4" rx="1" fill="{c}"/></g>' % (i * 45) for i in range(8))
        + '<circle cx="12" cy="12" r="7.2" fill="none" stroke="{c}" stroke-width="2.4"/>'),

    "plus": (
        '<g stroke="{c}" stroke-width="3" stroke-linecap="round">'
        '<line x1="12" y1="4.5" x2="12" y2="19.5"/>'
        '<line x1="4.5" y1="12" x2="19.5" y2="12"/></g>'),

    "minus": (
        '<line x1="4.5" y1="12" x2="19.5" y2="12" stroke="{c}" stroke-width="3" '
        'stroke-linecap="round"/>'),

    "boost": (
        '<path d="M13 2.5c.6 3.4-.7 5.2-2.3 6.8C9 11 7 12.8 7 15.6a5.6 5.6 0 0 0 '
        '11.2 0c0-2.4-1.2-4-2.4-5.6-.5 1-1.3 1.6-2.2 1.8.9-2.9.5-6.6-.6-9.3z" '
        'fill="{c}"/>'),

    "eco": (
        '<path d="M20 4c-9 0-14 3.4-14 9.4 0 2 .7 3.7 1.8 5C9.2 12.6 12.4 9.6 16.6 '
        '8.2c-3.6 2.2-6.4 5.6-7.8 9.8l-.9 3h2.4l.7-2.3C19 18.4 20 10.6 20 4z" '
        'fill="{c}"/>'),

    "frost": (
        '<g stroke="{c}" stroke-width="2.2" stroke-linecap="round">'
        '<line x1="12" y1="2.6" x2="12" y2="21.4"/>'
        '<line x1="3.9" y1="7.3" x2="20.1" y2="16.7"/>'
        '<line x1="3.9" y1="16.7" x2="20.1" y2="7.3"/>'
        '<g stroke-width="1.9">'
        '<path d="M9.4 4.9 12 6.7l2.6-1.8M9.4 19.1 12 17.3l2.6 1.8"/>'
        '<path d="M4.6 11 5.1 8l-3-.3M19.4 13l-.5 3 3 .3"/>'
        '<path d="M2.1 16.3l3-.3-.5-3M21.9 7.7l-3 .3.5 3"/></g></g>'),

    "power": (
        '<path d="M6.4 5.6a8.4 8.4 0 1 0 11.2 0" fill="none" stroke="{c}" '
        'stroke-width="2.4" stroke-linecap="round"/>'
        '<line x1="12" y1="2.4" x2="12" y2="11.4" stroke="{c}" stroke-width="2.6" '
        'stroke-linecap="round"/>'),

    "wifi": (
        '<g fill="none" stroke="{c}" stroke-width="2.4" stroke-linecap="round">'
        '<path d="M2.6 8.4a14 14 0 0 1 18.8 0"/>'
        '<path d="M6 12.4a9 9 0 0 1 12 0"/>'
        '<path d="M9.3 16.3a4.2 4.2 0 0 1 5.4 0"/></g>'
        '<circle cx="12" cy="20" r="1.9" fill="{c}"/>'),

    "prev": (
        '<path d="M15 4.5 7.5 12 15 19.5" fill="none" stroke="{c}" stroke-width="3" '
        'stroke-linecap="round" stroke-linejoin="round"/>'),

    "next": (
        '<path d="M9 4.5 16.5 12 9 19.5" fill="none" stroke="{c}" stroke-width="3" '
        'stroke-linecap="round" stroke-linejoin="round"/>'),
}

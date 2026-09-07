#include "nx_draw.h"

#include <math.h>

#include "nextion.h"

/* Verrou tenu ~140 ms pour ~60 disques (270° à r=52, cf. design/EDITOR-HOME.md) :
 * une attente courte suffit, ce n'est pas un transfert TFT. */
#define NX_DRAW_BUS_WAIT_MS 200

void nx_draw_arc(int cx, int cy, int r, int thickness,
                 int a_start, int a_end, uint16_t col565)
{
    if (thickness < 1) thickness = 1;
    int disc_r = thickness / 2;
    if (disc_r < 1) disc_r = 1;

    float sweep = (float)(a_end - a_start);
    if (sweep <= 0) return;

    /* Pas angulaire tel que deux disques consécutifs se recouvrent d'environ
     * moitié le long du tracé (pas de trou dans l'arc). */
    float step_deg = ((float)disc_r * 180.0f) / ((float)r * (float)M_PI);
    if (step_deg <= 0.0f || step_deg > sweep) step_deg = sweep;
    int steps = (int)(sweep / step_deg) + 1;

    if (!nx_bus_acquire(NX_DRAW_BUS_WAIT_MS)) return;

    for (int i = 0; i <= steps; i++) {
        float a = (float)a_start + sweep * (float)i / (float)steps;
        float rad = a * (float)M_PI / 180.0f;
        int x = cx + (int)lroundf((float)r * cosf(rad));
        int y = cy + (int)lroundf((float)r * sinf(rad));
        nx_cmd("cirs %d,%d,%d,%u", x, y, disc_r, (unsigned)col565);
    }

    nx_bus_release();
}

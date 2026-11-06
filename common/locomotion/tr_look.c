/* tr_look.c - THE LOOK: the lizard's draw-only state (blink, tongue, stride, fat)
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/terrarium/terrarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- P3b DRAW-ONLY: THE LOOK (tank.h fish_t lz_*; render_lizard.h lizard_pose_t) ----
 * What draw_lizard needs of a lizard that THE WALK does not keep, worked out each tick from what it does: the walk's
 * phase (by the ground it covers, so a planted foot stays put - its stride, species.h lizard_stride_px - the turn's
 * shuffle and the dig's scrabble by the clock; a lunge's phase is how far it has reached), the blink, the tongue's flicks
 * (often while it explores or looks at the other - FLICK -, now and then stalking or standing), and the fat reserve its
 * tail shows (meals and hunger, slowly). The engine never reads these, and nothing here draws on the tank's random
 * stream (each lizard's own hash of its uid and its count): the walk runs exactly as without it. */
#define LOOK_BLINK_S      0.18f      /* a blink: shut and open again */
#define LOOK_TONGUE_S     0.26f      /* a flick: out and back */
#define LOOK_FAT_TAU_S    180.0f     /* the tail's girth follows its meals this slowly */
static float look_hash01(const fish_t *f, uint32_t n) {
    uint32_t h = f->uid * 0x9E3779B1u ^ n * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(h & 0xffff) / 65535.0f;
}
void lizard_look(tank_t *t, fish_t *f, float dt) {
    const species_t *p = tank_species(t);
    const float stride = lizard_stride_px(&p->geom, tank_lizard_scale(f));
    if (f->pose == LZ_POSE_LUNGE) f->lz_phase = f->pose_s + f->strike_s > 0 ? clampf(f->pose_s / (f->pose_s + f->strike_s), 0, 1) : 1;   /* the reach: a lunge's or a dab's
                                                                                                  share done (its seconds so far, and left) */
    else {
        float cyc = f->speed > 0 ? f->speed * dt / (stride > 1 ? stride : 1)    /* the ground covered (along the slope) */
                  : f->pose == LZ_POSE_TURN ? 1.6f * dt                        /* the turn's shuffle */
                  : f->pose == LZ_POSE_DIG ? 2.4f * dt : 0;                    /* the scrabble */
        f->lz_phase += cyc;
        f->lz_phase -= floorf(f->lz_phase);
    }
    /* the blink: every 2.5..7 s; lz_blink_s under 0 while one is under way */
    if (f->lz_blink_s > 0) { f->lz_blink_s -= dt; if (f->lz_blink_s <= 0) f->lz_blink_s = -LOOK_BLINK_S; }
    else {
        f->lz_blink_s += dt;
        if (f->lz_blink_s >= 0) f->lz_blink_s = 2.5f + 4.5f * look_hash01(f, (uint32_t)(t->clock * 3.0));
    }
    { const float u = f->lz_blink_s < 0 ? 1 + f->lz_blink_s / LOOK_BLINK_S : 1; f->lz_blink = f->lz_blink_s < 0 ? 1 - fabsf(2 * u - 1) : 0; }
    /* the tongue: a flick, then the next by what it is doing */
    const bool flicks = f->pose == LZ_POSE_FLICK || f->pose == LZ_POSE_STALK || f->pose == LZ_POSE_STAND || f->pose == LZ_POSE_WALK
                        || f->pose == LZ_POSE_PEEK || f->pose == LZ_POSE_STANDOFF;
    if (f->lz_tongue_s > 0) { f->lz_tongue_s -= dt; if (f->lz_tongue_s <= 0 && flicks) f->lz_tongue_s = -LOOK_TONGUE_S; else if (f->lz_tongue_s <= 0) f->lz_tongue_s = 1; }
    else {
        f->lz_tongue_s += dt;
        if (f->lz_tongue_s >= 0) {
            const float r = look_hash01(f, (uint32_t)(t->clock * 5.0) + 7919u);
            f->lz_tongue_s = f->pose == LZ_POSE_FLICK ? 0.5f + 1.2f * r : f->pose == LZ_POSE_STALK || f->pose == LZ_POSE_PEEK ? 1.8f + 2.5f * r : 4.0f + 6.0f * r;
        }
    }
    f->lz_tongue = f->lz_tongue_s < 0 ? sinf(3.14159265f * (1 + f->lz_tongue_s / LOOK_TONGUE_S)) : 0;
    /* the fat reserve: fed and not hungry, a full tail; starving, a thin one */
    const float want = clampf(0.30f + 0.45f * (1 - f->hunger / 10) + 0.25f * clampf(f->eaten / 40.0f, 0, 1), 0.08f, 1);
    f->lz_fat = f->lz_fat <= 0 ? want : f->lz_fat + (want - f->lz_fat) * clampf(dt / LOOK_FAT_TAU_S, 0, 1);
}

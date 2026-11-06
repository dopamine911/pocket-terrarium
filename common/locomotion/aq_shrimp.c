/* aq_shrimp.c - the shrimp school (SD_ITEM_SHRIMP)
 * (issue #4 phase 3: moved out of tank.c verbatim; see tank.h for the rules it plays by) */
#include "tank.h"
#include "tank_events.h"
#include "tank_priv.h"
#include "worlds/aquarium/aquarium_priv.h"
#include "pt_util.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the shrimp school (SD_ITEM_SHRIMP; tank.h has the rules) ----
 * A loose school steering to one goal - a spot at the foot of the grass most
 * of the time, now and then out on the sand or up in open water - each shrimp
 * at its own offset from it, keeping about a body length from its neighbours
 * (Strato: "space them out a bit more so they overlap a tad less"). A pellet
 * resting on the floor becomes the goal and they crowd in to peck it. They
 * turn like the fish: a committed turn, then a settle. Tuned against the
 * 2026-09-29 mockup Strato approved. */
#define SHRIMP_CRUISE    12.0f        /* px/s */
#define SHRIMP_DART      58.0f        /* px/s, the tail-flick escape */
#define SHRIMP_FLEE_X    3.0f         /* the school's flight from the crab: 36 px/s, three times its cruise (Strato: "faster than their slow usual swim") */
#define SHRIMP_SEP_X     18.0f        /* personal space, side to side (a body and a bit) ... */
#define SHRIMP_SEP_Y     11.0f        /* ... and up and down (7 stacked them in towers on the glass) */
#define SHRIMP_TAP_R     30.0f        /* a fingertip on a shrimp: its card */
#define SHRIMP_HEAD      7.0f         /* the head's offset from the body's centre, along the facing */
static float shrimp_hash(int i, int k) {  /* a fixed 0..1 per shrimp: its place in the school (no RNG draw) */
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)k * 40503u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xffff) / 65535.0f;
}
bool tank_shrimp_refusing(const tank_t *t) { return tank_algae_cover(t) > SHRIMP_REFUSE_COVER; }
static void shrimp_spawn(tank_t *t, int i, float x) {
    shrimp_t *q = &t->shrimp[i];
    memset(q, 0, sizeof *q);
    q->x = x; q->y = FOOD_FLOOR_Y - 6 - shrimp_hash(i, 3) * 14;
    q->facing = shrimp_hash(i, 4) < 0.5f ? -1 : 1; q->yaw = q->facing;
    q->ph = shrimp_hash(i, 5) * 6.28f;
}
void tank_shrimp_place(tank_t *t, int n) {
    if (n < 0) n = 0;
    if (n > SHRIMP_MAX) n = SHRIMP_MAX;
    int wide = 0; float wbest = -1, x0 = 0, x1 = 0;           /* at the foot of the widest bed, clear of the glass */
    for (int b = 0; b < tank_veg_beds(t); b++) { float a, z; tank_veg_bed(t, b, &a, &z, NULL, NULL); if (z - a > wbest) { wbest = z - a; wide = b; } }
    tank_veg_bed(t, wide, &x0, &x1, NULL, NULL);
    float cx = clampf((x0 + x1) * 0.5f, TANK_FX0 + 60, TANK_FX1 - 60);
    for (int i = 0; i < n; i++) shrimp_spawn(t, i, cx + (shrimp_hash(i, 1) - 0.5f) * (40 + n * 9));
    t->shrimp_n = (uint8_t)n;
    t->shrimp_tx = cx; t->shrimp_ty = FOOD_FLOOR_Y - 12; t->shrimp_tt = 4;
}
/* the school's next goal. Mostly the foot of the grass; now and then out on
 * the sand; and now and then a slow DRIFT UP a bed to the top of its canopy
 * (or at least SHRIMP_RISE_MIN above the floor when the grass is cut short),
 * a while up there, then back down - Strato, 2026-09-29: "you dont get to see
 * them much ... i dont want them swimming all over the tank all the time but a
 * chance for them to randomly drift up to at least the top of the sea grass".
 * About a quarter of their time goes on these trips. Never while refusing food. */
#define SHRIMP_RISE_P    0.06f         /* of the goals picked (0.10: up off the bottom ~45% of the time) */
#define SHRIMP_RISE_MIN  60.0f         /* px above the floor, at the least */
static void shrimp_goal(tank_t *t, bool refusing) {
    int beds = tank_veg_beds(t);
    float r = tank_randf(t, 0, 1);
    if (!refusing && r < SHRIMP_RISE_P && beds > 0) {              /* up the grass to the canopy */
        int b = (int)tank_randf(t, 0, beds - 0.001f);
        float x0, x1, top; tank_veg_bed(t, b, &x0, &x1, &top, NULL);
        t->shrimp_tx = x1 - x0 > 16 ? tank_randf(t, x0 + 8, x1 - 8) : (x0 + x1) * 0.5f;
        t->shrimp_ty = fminf(top + tank_randf(t, -8, 10), FOOD_FLOOR_Y - SHRIMP_RISE_MIN);
        t->shrimp_tt = tank_randf(t, 22, 32);                           /* the drift up is slow; then a while at the top */
        { float r2 = depth_hash(t, t->shrimp_tx, t->shrimp_ty, 0x5a1u);   /* SWIM DEPTH: up a bed, through it mostly */
          t->shr_gdepth_want = r2 < 0.6f ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT; t->shr_pdepth_want = FISH_DEPTH_FRONT; }
        return;
    }
    if ((refusing || r < SHRIMP_RISE_P + 0.62f) && beds > 0) {     /* the grass: the school's cover */
        int b = (int)tank_randf(t, 0, beds - 0.001f);
        float x0, x1; tank_veg_bed(t, b, &x0, &x1, NULL, NULL);
        t->shrimp_tx = x1 - x0 > 16 ? tank_randf(t, x0 + 8, x1 - 8) : (x0 + x1) * 0.5f;
    } else t->shrimp_tx = tank_randf(t, TANK_FX0 + 40, TANK_FX1 - 40);         /* out on the sand */
    t->shrimp_ty = refusing || tank_randf(t, 0, 1) < 0.8f ? FOOD_FLOOR_Y - tank_randf(t, 12, 26)
                                                          : FOOD_FLOOR_Y - tank_randf(t, 30, 64);   /* a swim up into open water */
    t->shrimp_tt = tank_randf(t, 6, 11);
    {   /* SWIM DEPTH (0.3.9.4): at the foot of the grass, tucked in behind or through it most of the time; out on the sand and in
           open water, in front; now and then behind the pieces on the way */
        float r2 = depth_hash(t, t->shrimp_tx, t->shrimp_ty, 0x5a2u), r3 = depth_hash(t, t->shrimp_ty, t->shrimp_tx, 0x5a3u);
        bool cover = false; int nb = tank_veg_beds(t);
        for (int b = 0; b < nb && !cover; b++) { float x0, x1; tank_veg_bed(t, b, &x0, &x1, NULL, NULL); cover = t->shrimp_tx >= x0 && t->shrimp_tx <= x1; }
        t->shr_gdepth_want = !cover ? FISH_DEPTH_FRONT : r2 < 0.35f ? FISH_DEPTH_BEHIND : r2 < 0.75f ? FISH_DEPTH_THROUGH : FISH_DEPTH_FRONT;
        t->shr_pdepth_want = r3 < (cover ? 0.25f : 0.10f) ? FISH_DEPTH_BEHIND : FISH_DEPTH_FRONT;
    }
}
/* the grass's top over x: the bed under it, or the beds' average over open
 * sand. A pellet that has sunk below it is the shrimp's to take, still falling
 * or resting on the floor (Strato, 2026-09-29: "if food falls below the
 * seagrass canopy its fair game for shrimp even as its still floating downward") */
static float shrimp_canopy_y(const tank_t *t, float x) {
    float sum = 0; int nb = tank_veg_beds(t);
    for (int b = 0; b < nb; b++) {
        float x0, x1, top; tank_veg_bed(t, b, &x0, &x1, &top, NULL);
        if (x >= x0 && x <= x1) return top;
        sum += top;
    }
    return nb ? sum / nb : FOOD_FLOOR_Y;
}
static bool shrimp_food_ok(const tank_t *t, const food_t *p) {
    if (t->cr_claim >= 0 && p == &t->food[t->cr_claim]) return false;   /* the crab's (0.3.9.1): it shooed them off it, or eats it */
    if (t->shrimp_avoid_s > 0 && fabsf(p->x - t->shrimp_avoid_x) < CR_SHOO_ZONE_PX) return false;   /* ... and its neighbours: the area they were shooed from */
    return p->alive && (p->floor_s > 0 || p->y >= shrimp_canopy_y(t, p->x));
}
void shrimp_tick(tank_t *t, float dt) {
    if (!(t->sd_unlocks & SD_ITEM_SHRIMP)) return;
    if (t->shrimp_n < SHRIMP_START) tank_shrimp_place(t, SHRIMP_START);
    bool refusing = tank_shrimp_refusing(t);
    if (t->shrimp_cool > 0) t->shrimp_cool -= dt;
    int n = t->shrimp_n;
    float cx = 0, cy = 0;                                          /* the school's middle */
    for (int i = 0; i < n; i++) { cx += t->shrimp[i].x; cy += t->shrimp[i].y; }
    cx /= n; cy /= n;
    /* a newcomer: ten pellets eaten and the cooldown out - it steps out of the grass by the school */
    if (t->shrimp_food >= SHRIMP_PER_JOIN && t->shrimp_cool <= 0 && n < SHRIMP_MAX) {
        shrimp_spawn(t, n, cx + (shrimp_hash(n, 1) - 0.5f) * 20);
        t->shrimp_n = (uint8_t)++n;
        t->shrimp_food = 0; t->shrimp_cool = SHRIMP_COOLDOWN_S;
    }
    t->shrimp_tt -= dt;
    if (t->shrimp_tt <= 0) shrimp_goal(t, refusing);
    const bool flee = t->shrimp_flee > 0;                         /* running from the crab: no food, all speed (0.3.9.1) */
    if (flee) t->shrimp_flee -= dt;
    if (t->shrimp_avoid_s > 0) {                                   /* the area they were shooed from: their goals keep out of it */
        t->shrimp_avoid_s -= dt;
        const float side = t->shrimp_avoid_side, edge = t->shrimp_avoid_x + side * (CR_SHOO_ZONE_PX + 20);
        if ((t->shrimp_tx - edge) * side < 0) t->shrimp_tx = clampf(edge, TANK_FX0 + 40, TANK_FX1 - 40);   /* on the shoo's side, past the area */
    }
    int fp = -1; float best = 1e9f;                                /* the pellet in reach nearest the school */
    if (!refusing && !flee)
        for (int k = 0; k < MAX_FOOD; k++) {
            const food_t *p = &t->food[k];
            if (!shrimp_food_ok(t, p)) continue;
            float d = fabsf(p->x - cx) + fabsf(p->y - cy) * 0.5f;
            if (d < best) { best = d; fp = k; }
        }
    t->shrimp_fp = (int8_t)fp;
    {   /* SWIM DEPTH: the school's depth - in front chasing a pellet, hiding when fleeing the crab, else its goal's; taken up
           only with the whole school clear of every bed / piece */
        float bx0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (int i = 0; i < n; i++) { bx0 = fminf(bx0, t->shrimp[i].x - 12); bx1 = fmaxf(bx1, t->shrimp[i].x + 12); by1 = fmaxf(by1, t->shrimp[i].y + 6); }
        uint8_t gw = fp >= 0 ? FISH_DEPTH_FRONT : flee ? FISH_DEPTH_BEHIND : t->shr_gdepth_want;
        uint8_t pw = fp >= 0 ? FISH_DEPTH_FRONT : flee ? FISH_DEPTH_BEHIND : t->shr_pdepth_want;
        if (t->shr_gdepth != gw && box_clear_beds(t, bx0, bx1, by1) && pieces_keep(t, bx0, bx1, by1, t->shr_gdepth, t->shr_pdepth, gw, t->shr_pdepth)) t->shr_gdepth = gw;
        if (t->shr_pdepth != pw && pieces_keep(t, bx0, bx1, by1, t->shr_gdepth, t->shr_pdepth, t->shr_gdepth, pw)) t->shr_pdepth = pw;
    }
    float gx = fp >= 0 ? t->food[fp].x : t->shrimp_tx;
    float gy = fp < 0 ? t->shrimp_ty : t->food[fp].floor_s > 0 ? FOOD_FLOOR_Y - 2 : t->food[fp].y;   /* on the floor, or still sinking */
    float spread = 40 + n * 9;                                    /* the school's width: 76 px for 4, 130 for 10 */
    /* the whole school fits between the side glass: a goal by the wall (a bed
       against it, a pellet in the corner) keeps them spread, not piled up on
       the glass; the ones nearest a corner pellet still reach it */
    float half = (fp >= 0 ? spread * 0.45f : spread) * 0.5f + 16;
    gx = clampf(gx, TANK_FX0 + half, TANK_FX1 - half);
    for (int i = 0; i < n; i++) {
        shrimp_t *q = &t->shrimp[i];
        q->ph += dt; q->pecking = 0;
        if (q->dart > 0) {
            q->dart -= dt;
            float k = powf(0.2f, dt); q->vx *= k; q->vy *= k;
        } else {
            float ox = (shrimp_hash(i, 1) - 0.5f) * spread, oy = (shrimp_hash(i, 2) - 0.5f) * 24;
            float tx = gx + (fp >= 0 ? ox * 0.45f : ox), ty = gy + (fp >= 0 ? 0 : oy);
            float dx = tx - q->x, dy = ty - q->y, d = sqrtf(dx * dx + dy * dy) + 1e-3f;
            bool chase = fp >= 0;                                       /* food in reach: they hurry, up or down alike */
            float sp = fminf(SHRIMP_CRUISE * (flee ? SHRIMP_FLEE_X : chase ? 1.6f : 1.0f), d * 0.9f);
            float wx = dx / d * sp, wy = dy / d * sp * (chase ? 1.0f : 0.7f);
            float sx = fp >= 0 ? SHRIMP_SEP_X * 0.6f : SHRIMP_SEP_X, sy = fp >= 0 ? SHRIMP_SEP_Y * 0.6f : SHRIMP_SEP_Y;
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                float nx = (q->x - t->shrimp[j].x) / sx, ny = (q->y - t->shrimp[j].y) / sy, e = sqrtf(nx * nx + ny * ny);
                if (e < 1 && e > 0) { wx += nx / e * (1 - e) * 22; wy += ny / e * (1 - e) * 9; }
            }
            float a = fminf(1, dt * (flee ? 6.0f : 2.5f));                 /* (fleeing: up to speed at once) */
            q->vx += (wx - q->vx) * a; q->vy += (wy - q->vy) * a;
            float hx = q->x + SHRIMP_HEAD * q->facing;
            for (int k = 0; k < MAX_FOOD; k++) {                       /* a pellet at its head */
                food_t *p = &t->food[k];
                if (!shrimp_food_ok(t, p) || fabsf(p->x - hx) > 8 || fabsf(p->y - q->y) > 9) continue;
                if (refusing) {                                        /* too much algae: it turns its back on it */
                    q->facing = (int8_t)(q->x > p->x ? 1 : -1); q->behind = -0.6f;
                    q->vx = q->facing * 6.0f;
                } else if (k == fp) {
                    q->pecking = 1; q->vx *= 0.5f;
                    q->vy = p->floor_s > 0 ? q->vy * 0.5f : 8.0f;           /* a falling pellet: it sinks with it */
                    p->nibbled += dt;
                }
                break;
            }
        }
        q->x += q->vx * dt;
        q->y += q->vy * dt + sinf(q->ph * 2.1f) * dt * 1.2f;
        q->x = clampf(q->x, TANK_FX0 + 12, TANK_FX1 - 12);
        q->y = clampf(q->y, 40, FOOD_FLOOR_Y - 1);
#if defined(TANK_ROUND) || defined(TANK_CORNER_R)
        tank_glass_clamp(&q->x, &q->y, 10);          /* up a tall frond by the wall, the bowl closes in */
#endif
        if (q->dart <= 0) {                                             /* the committed turn */
            int8_t want = q->vx > 1.5f ? 1 : q->vx < -1.5f ? -1 : q->facing;
            if (want != q->facing) { q->behind += dt; if (q->behind > 0.35f) { q->facing = want; q->behind = -0.6f; } }
            else q->behind = fminf(0, q->behind + dt);
        }
        q->yaw += clampf(q->facing - q->yaw, -5 * dt, 5 * dt);
    }
    if (fp >= 0 && t->food[fp].nibbled >= SHRIMP_PECK_S) {             /* eaten: one toward the next shrimp */
        t->food[fp].alive = false;
        if (t->shrimp_food < SHRIMP_PER_JOIN) t->shrimp_food++;       /* stops at ten while the cooldown runs */
        t->shrimp_eaten++;
    }
}
/* asleep: nothing is eaten, so nothing joins; the cooldown runs on lived time */
void shrimp_sleep(tank_t *t, float seconds) {
    if (!(t->sd_unlocks & SD_ITEM_SHRIMP)) return;
    t->shrimp_cool = fmaxf(0, t->shrimp_cool - seconds);
}
void shrimp_scatter(tank_t *t, float x, float y, float radius) {
    if (!(t->sd_unlocks & SD_ITEM_SHRIMP)) return;
    bool any = false;
    for (int i = 0; i < t->shrimp_n; i++) {
        shrimp_t *q = &t->shrimp[i];
        float dx = q->x - x, dy = q->y - y, d = sqrtf(dx * dx + dy * dy);
        if (d >= radius) continue;
        if (d < 1) { dx = q->facing ? -q->facing : 1; dy = 0; d = 1; }
        q->dart = 0.55f; q->vx = dx / d * SHRIMP_DART; q->vy = dy / d * SHRIMP_DART * 0.7f;
        q->facing = (int8_t)(q->vx > 0 ? -1 : 1); q->yaw = q->facing;   /* tail first: they flick backward */
        q->behind = -0.8f; any = true;
    }
    if (any) t->shrimp_tt = fminf(t->shrimp_tt, 3.0f);                /* and regroup somewhere new */
}

bool tank_shrimp_hit(const tank_t *t, float x, float y) {
    if (!(t->sd_unlocks & SD_ITEM_SHRIMP)) return false;
    for (int i = 0; i < t->shrimp_n; i++)
        if (tank_dist(t->shrimp[i].x, t->shrimp[i].y, x, y) <= SHRIMP_TAP_R) return true;
    return false;
}
int tank_shrimp_tap(tank_t *t, float x, float y) {
    tank_handled(t);
    if (t->shrimp_tap_t > TAP_WINDOW) t->shrimp_taps = 0;
    if (t->shrimp_taps < 255) t->shrimp_taps++;
    t->shrimp_tap_t = 0;
    if (t->shrimp_taps == 3) tank_startle(t, x, y);
    return t->shrimp_taps;
}

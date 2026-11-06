/* fish.c - the fish body's drawing (issue #4 phase 3: moved verbatim out of render.c).
 *
 * The fish drawn procedurally: its three body shapes and fin sets (the genes of 0.4), its turn, its trail's bend, its markings, the
 * previews' fit. draw_fish is the tank's (the aquarium's scene calls it), draw_fish_core the one the previews call too. */
#include "render_core.h"
#include "bodies/fish/fish.h"
#include "pt_util.h"
#include <stdlib.h>

/* the turn (2026-09-29, was a roll that flattened the fish to a line): the
 * fish's yaw (tank.c) FORESHORTENS it along its length - side-on at +-1,
 * head-on at 0 - and the sign mirrors it, so its back stays up. The head's
 * yaw leads and the tail's follows, blended along the body, so mid-turn it
 * bends. Eased here so the swing starts and lands softly. */
static float yaw_ease(float y) { return y * (1.5f - 0.5f * y * y); }
#define YAW_MIN 0.06f                    /* the outline never folds to a line (the body's
                                         * thickness is drawn on its own: FISH_THICK) */
#define FISH_THICK 4.2f                  /* half the body's thickness, local px: turned toward
                                         * the glass a fish shows a rounded oval, not a card */
/* the yaw at body point lx: the tail's at the tail tip (-26), the head's at the nose (16) */
static float yaw_at(float yh, float yt, float lx) {
    float u = (lx + 26) * (1.0f / 42);
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    float s = yt + (yh - yt) * u;
    if (s > -YAW_MIN && s < YAW_MIN) s = s < 0 ? -YAW_MIN : YAW_MIN;
    return s;
}

/* the trail's bend (0.4): a body point lx behind bb sits on an arc of
 * curvature k, its frame turned by th = k (bb - lx) - so a tail lagging a
 * turn curves the rear body smoothly instead of the whole fish swinging
 * rigid on its middle. Past the body's end (e) a fin curves on at k x tr:
 * a classic's tail fin rides stiff off the body, a long veil keeps
 * flowing. Small-angle forms (th held under 1 rad): a few multiplies a
 * vertex, nothing a pixel. k = 0 is the rigid fish exactly */
typedef struct { float bb, e, k, tr; } bend_t;
static inline void bend_pt(const bend_t *b, float lx, float ly, float *ox, float *oy) {
    if (lx >= b->bb || b->k == 0) { *ox = lx; *oy = ly; return; }
    float s = b->bb - lx, th, x, y;
    if (lx >= b->e) {                                    /* on the body: one arc */
        th = b->k * s;
        x = lx + s * th * th * (1 / 6.0f); y = -s * th * 0.5f;
    } else {                                             /* past it: the arc to e, then on at k x tr */
        float s0 = b->bb - b->e, t0 = b->k * s0, kr = b->k * b->tr, r = b->e - lx;
        th = t0 + kr * r;
        x = b->bb - s0 + s0 * t0 * t0 * (1 / 6.0f) - r + (t0 * t0 * r + t0 * kr * r * r + kr * kr * r * r * r * (1 / 3.0f)) * 0.5f;
        y = -s0 * t0 * 0.5f - (t0 * r + kr * r * r * 0.5f);
    }
    th = th > 1 ? 1 : th < -1 ? -1 : th;
    *ox = x - ly * th; *oy = y + ly * (1 - th * th * 0.5f);
}
static inline float bend_x(const bend_t *b, float lx, float ly) { float x, y; bend_pt(b, lx, ly, &x, &y); return x; }
static inline float bend_y(const bend_t *b, float lx, float ly) { float x, y; bend_pt(b, lx, ly, &x, &y); return y; }

/* the body shapes (SHAPE_*, 0.4; docs/genetics-0.4.md and its sketch): the
 * outline, sampled from the prototype's quadratic silhouette for CLASSIC;
 * where the tail joins (root); the half-height the fins and the pattern scale
 * by (h); how far the fins reach past the body (fh: a slender fish keeps a
 * real sail, a bulbous one's stays in proportion); the fins' x scale (k: the
 * body's half-length over CLASSIC's, so a dorsal sits on the back it belongs
 * to); the eye; the bars' center and spacing (bulbous bars spread clear of
 * the eye). (The tail beat's pace by form: tank.c, tank_fish_beat_rate - the beat runs in the swim since 0.4) */
typedef struct { float x[10], y[10], root, h, fh, k, eye_x, eye_y, bar_x, bar_dx; } fish_form_t;
static const fish_form_t FORMS[SHAPE_N] = {
    /* CLASSIC: every fish before 0.4 */
    { { 16, 10, 2, -8, -15, -18, -15, -8, 2, 10 }, { 0, -7, -10, -9, -6, 0, 6, 9, 10, 7 },
      -11, 10, 10, 1.0f, 9, -3, -3, 6 },
    /* SLENDER: long and low, a pointed nose (a danio) */
    { { 21, 14, 4, -8, -17, -21, -17, -8, 4, 14 }, { 0, -4.5f, -6.5f, -6, -4, 0, 4, 6, 6.5f, 4.5f },
      -15, 6.5f, 10, 1.235f, 13, -2, -3.7f, 7.4f },
    /* BULBOUS: tall and round (a discus) */
    { { 13, 9, 1, -6, -11, -13, -11, -6, 1, 9 }, { 0, -11, -15, -14, -9, 0, 9, 14, 15, 11 },
      -9, 15, 12, 0.765f, 7, -4, -4, 6.5f },
};

/* clip a convex polygon (local coords) to the side of the line a->b where
 * (b - a) x (p - a) <= 0 - for a line drawn downward, the side toward the nose */
static int clip_side(const float *xs, const float *ys, int n, float ax, float ay, float bx, float by,
                     float *ox, float *oy) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float si = (bx - ax) * (ys[i] - ay) - (by - ay) * (xs[i] - ax);
        float sj = (bx - ax) * (ys[j] - ay) - (by - ay) * (xs[j] - ax);
        if (si <= 0) { ox[m] = xs[i]; oy[m] = ys[i]; m++; }
        if ((si <= 0) != (sj <= 0)) {
            float u = si / (si - sj);
            ox[m] = xs[i] + (xs[j] - xs[i]) * u; oy[m] = ys[i] + (ys[j] - ys[i]) * u; m++;
        }
    }
    return m;
}

/* a fish's fins as polygons in its local coords, behind-the-body order:
 * the arrangement grows in at ADULT (younger fish: the plain tail), the
 * elder's are longer. tail / lag: the beat's phase (0, 0 = the still pose
 * fish_fit measures). Returns the count (at most 4). */
typedef struct { int n; float x[4], y[4]; } fin_poly_t;
static int fish_fins(const fish_form_t *fm, int fins, stage_t stage, float tail, float lag, fin_poly_t *o) {
    const float H = fm->h, FH = fm->fh, K = fm->k, R = fm->root;
    bool elder = stage == STAGE_ELDER, grown = stage >= STAGE_ADULT;
    float tail_len = elder ? 1.22f : 1.0f;
    int m = 0;
    if (!grown) fins = FINS_CLASSIC;
#define FIN(N, LX, LY) do { const float lx_[N] = LX, ly_[N] = LY; o[m].n = N; \
        for (int i_ = 0; i_ < N; i_++) { o[m].x[i_] = lx_[i_]; o[m].y[i_] = ly_[i_]; } m++; } while (0)
#define L(...) { __VA_ARGS__ }
#define TL(d) (R - (d) * tail_len)                     /* a point d px behind the tail root, the elder's longer */
    if (fins == FINS_CLASSIC) {
        /* tail fin (triangle, flaps with tail phase) */
        FIN(3, L(R, (R - 15) * tail_len, (R - 15) * tail_len), L(0, -10 - tail * 8, 10 + tail * 8));
        /* dorsal crest: adults a small fin, elders a taller one */
        if (grown) { float b = -H * 8 / 10, d = b - FH * (elder ? 9 : 5) / 10;   /* exact for CLASSIC: -8, -13 / -17 */
                     FIN(3, L(6 * K, -2 * K, -9 * K), L(b, d, b)); }
    } else if (fins == FINS_VEIL) {
        /* a long two-lobed tail that trails the beat, a trailing anal fin, a low dorsal fold */
        FIN(4, L(R, TL(10), TL(30), TL(26)), L(0, -7 - tail * 3, -13 - lag * 9, -1 - lag * 3));
        FIN(4, L(R, TL(26), TL(32), TL(12)), L(0, 1 + lag * 3, 16 + lag * 9, 8 + tail * 3));
        float a = elder ? 1.3f : 1.0f;
        FIN(4, L(2 * K, -6 * K, -14 * K - lag * 2, -4 * K), L(0.9f * H, 0.9f * H, 0.9f * H + a * FH, 0.9f * H + 0.6f * FH));
        FIN(3, L(4 * K, -8 * K, -12 * K), L(-0.9f * H, -0.9f * H - (elder ? 0.6f : 0.35f) * FH, -0.8f * H));
    } else {
        /* SAIL: a small lyre tail; a tall swept-back dorsal sail and its mirror below */
        FIN(3, L(R, TL(11), TL(7)), L(0, -9 - tail * 5, 0));
        FIN(3, L(R, TL(11), TL(7)), L(0, 9 + tail * 5, 0));
        float e = elder ? 1.15f : 1.0f;
        FIN(4, L(8 * K, 0, -12 * K + lag * 1.5f, -12 * K), L(-0.85f * H, -0.85f * H - 1.45f * e * FH, -0.85f * H - 1.05f * e * FH, -0.7f * H));
        FIN(4, L(4 * K, -4 * K, -12 * K + lag * 1.5f, -12 * K), L(0.85f * H, 0.85f * H + 1.15f * e * FH, 0.85f * H + 0.85f * e * FH, 0.7f * H));
    }
#undef TL
#undef L
#undef FIN
    return m;
}

/* the trail's drag on the fins (0.4): s > 0 a burst - the tail streams
 * out long and narrow, a dorsal or anal fin lays back along the body; s < 0
 * the fish braking - the tail spreads, the tall fins swing a little forward.
 * d: the tail's lag through a climb or dive (the bend's gap) - a tall fin's
 * reach past the body leans by it too, as the tail does, so a sail is not a
 * board bolted on. The fins move, never the body they grow from */
static void fins_drag(const fish_form_t *fm, fin_poly_t *fp, int nf, float s, float d) {
    const float R = fm->root, edge = 0.9f * fm->h, as = fabsf(s);
    for (int k = 0; k < nf; k++)
        for (int i = 0; i < fp[k].n; i++) {
            float *x = &fp[k].x[i], *y = &fp[k].y[i];
            if (*x < R) {                                       /* the tail */
                *y *= 1 - 0.3f * s;
                *x += (*x - R) * 0.14f * s;
            } else if (fabsf(*y) > edge) {                      /* a tall fin, past the body's edge */
                float e = fabsf(*y) - edge;
                *x -= e * 0.55f * s + (*y > 0 ? e : -e) * d;
                *y -= (*y > 0 ? 1 : -1) * e * 0.18f * as;
            }
        }
}

/* the previews' fit (0.4): a page draws a fish in the room a CLASSIC fish of
 * the same stage takes (the milestones rows are 40 px apart - 34 on the bowl
 * - and a bulbous sail fish is ~2.5x a classic's height). The still pose's
 * box, scaled to fit inside a full-grown (ELDER) classic's (times `room`:
 * FISH_ROOM_* - a card has space a row does not) and centred where a classic
 * of its stage would be; never enlarged; a classic fish is untouched (no
 * shift - exact). */
void fish_box(int shape, int fins, stage_t stage, float b[4]) {
    const fish_form_t *fm = &FORMS[shape];
    b[0] = b[2] = 1e9f; b[1] = b[3] = -1e9f;
    fin_poly_t fp[4]; int nf = fish_fins(fm, fins, stage, 0, 0, fp);
    for (int k = -1; k < nf; k++) {
        const float *xs = k < 0 ? fm->x : fp[k].x, *ys = k < 0 ? fm->y : fp[k].y;
        int n = k < 0 ? 10 : fp[k].n;
        for (int i = 0; i < n; i++) {
            b[0] = fminf(b[0], xs[i]); b[1] = fmaxf(b[1], xs[i]);
            b[2] = fminf(b[2], ys[i]); b[3] = fmaxf(b[3], ys[i]);
        }
    }
}
void fish_fit(fish_t *f, float x, float y, float size, float room) {
    f->x = x; f->y = y; f->size = size;
    int sh = f->shape < SHAPE_N ? f->shape : 0, fi = f->fins < FINS_N ? f->fins : 0;
    if (sh == SHAPE_CLASSIC && fi == FINS_CLASSIC) return;
    float b[4], r[4], e[4];
    fish_box(sh, fi, f->stage, b); fish_box(SHAPE_CLASSIC, FINS_CLASSIC, f->stage, r);
    /* the room is a full-grown classic's - the ELDER's box, the biggest a
       classic ever is - not a classic of this fish's own stage: a fry only
       shrinks if it would really overflow (Strato 10-04: a bulbous sail fry
       fitted into a classic fry's box was a speck in its row) */
    fish_box(SHAPE_CLASSIC, FINS_CLASSIC, STAGE_ELDER, e);
    float k = fminf(1, room * fminf((e[1] - e[0]) / (b[1] - b[0]), (e[3] - e[2]) / (b[3] - b[2])));
    f->size = size * k;
    f->x = x + (r[0] + r[1]) * 0.5f * size - (b[0] + b[1]) * 0.5f * f->size;
    f->y = y + (r[2] + r[3]) * 0.5f * size - (b[2] + b[3]) * 0.5f * f->size;
}

/* The fish's body is a ledger of its life: stage sets size (tank.c) and fin
 * elaboration, hunger drains saturation, and earned markings stay:
 *   elder -> a longer tail and a taller dorsal. All procedural, zero flash.
 * Its genes (0.4) pick the body, the fins and the pattern; CLASSIC / CLASSIC
 * / BARS draws exactly what every fish drew before. */
/* `mark` scales the pattern: 1 in the tank (every stage wears the same
 * 2 x 5 px bars), the preview's size so it reads on a big fish */
/* `floor_y`: the sand line under a fish in the tank (FISH_FLOOR_Y; the
 * previews pass none) - nose up, a fish close to it lifts off level */
/* `beat`: the tail beat's phase - in the tank the fish's own (fish_t.beat, run on by the swim), in a preview the clock's at
 * the preview's pace (draw_still) */
void draw_fish_core(ctx_t *c, const fish_t *f, float beat, bool asleep, float mark, float floor_y) {
    const fish_form_t *fm = &FORMS[f->shape < SHAPE_N ? f->shape : 0];
    const float H = fm->h;
    int fins = f->fins < FINS_N ? f->fins : 0;
    float ph = beat + f->wander, amp = 0.32f + f->speed * 0.007f;
    float tail = sinf(ph) * amp;
    float lag = sinf(ph - 1.1f) * amp;                 /* the long fins follow the beat a moment late */
    float stress = f->stress / 10.0f;
    float pale = f->hunger > 7 ? (f->hunger - 7) / 3.0f * 0.35f : 0;   /* hungry = washed out */
    /* pitch: the heading folded to face right (the climb or dive), then the
     * yaw scales x: X = s(lx) * (lx cos p - ly sin p), Y = lx sin p + ly cos p.
     * The BODY tilts less than the path: a fish rises and sinks on its fins
     * with its back near level, so the travel angle is eased toward a cap of
     * ~35 deg (~55 at a dash) - never nose-straight-up (Strato 09-29: "too
     * much time in a vertical position") */
    float pmax = 0.61f + 0.35f * fminf(fmaxf((f->speed - 35) * (1 / 40.0f), 0), 1);
    /* the trail (0.4, tank_fish_trail): the body's pitch a moment behind the
     * heading's, the tail's further behind it - drawn as a bend of the rear
     * body (below). The springs' gaps are read against the heading, so a
     * fish posed by hand (a page, a card) is never drawn off its heading */
    float q = atan2f(sinf(f->heading), fabsf(cosf(f->heading)));
    float qb = q + fminf(fmaxf(f->pitch_b - q, -0.6f), 0.6f), qt = qb + fminf(fmaxf(f->pitch_t - f->pitch_b, -0.55f), 0.55f);
    float p = pmax * tanhf(0.8f * qb / pmax), pt = pmax * tanhf(0.8f * qt / pmax);
    /* off the sand (2026-10-24, Strato: a fish leaving the pellets on the
     * floor swung its tail through the sand and a castle's footing): nose
     * up, the tail swings DOWN - ~30 local px under the centre at a dash, an
     * elder's tip even further. Close to the floor the climb starts LEVEL
     * and tilts as the fish rises: level with the tail's flap just clear of
     * the sand (16 local px of water under the centre), the full tilt 20 px
     * higher. A fish rises off the bottom on its fins, back level. The
     * painter's window stops at the sand line too (draw_fish), so whatever
     * still reaches it brushes the floor instead of showing through it. */
    if (p < 0 && floor_y < 1e8f) {
        float k = ((floor_y - f->y) / f->size - 16) * (1 / 20.0f);
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        p *= k; pt *= k;
    }
    float cp = cosf(p), sp = sinf(p);
    /* the bend: behind bb the spine arcs, its angle growing to the gap
     * (pt - p) at the body's tail end and on along a long tail (bend_x/y) */
    float tr = tank_form_trail(f);
    bend_t bd = { fm->x[0] * 0.25f, fm->x[5], 0, 0.7f * tr };
    bd.k = (pt - p) / (bd.bb - bd.e);
    /* a burst streams the long fins back, a stop lets them billow forward */
    float surge = fminf(fmaxf((f->speed - f->speed_t) * (1 / 20.0f), -1), 1) * tr;
    float yh = yaw_ease(f->yaw), yt = yaw_ease(f->yaw_tail);
    uint32_t body = rgb_mix(rgb_mix(f->color, 0xf25b65, stress * 0.22f), 0x7a8a8e, pale);
    uint32_t fin  = rgb_mix(rgb_mix(f->fin, 0xf25b65, stress * 0.18f), 0x5a6a6e, pale);
#define BX(lx, ly) bend_x(&bd, lx, ly)
#define BY(lx, ly) bend_y(&bd, lx, ly)
#define TX(lx, ly) (f->x + yaw_at(yh, yt, lx) * (BX(lx, ly) * cp - BY(lx, ly) * sp) * f->size)
#define TY(lx, ly) (f->y + (BX(lx, ly) * sp + BY(lx, ly) * cp) * f->size)
    {
        fin_poly_t fp[4]; int nf = fish_fins(fm, fins, f->stage, tail, lag, fp);
        if (surge != 0 || pt != p) fins_drag(fm, fp, nf, surge, (pt - p) * tr);
        for (int k = 0; k < nf; k++) {
            float xs[4], ys[4];
            for (int i = 0; i < fp[k].n; i++) { xs[i] = TX(fp[k].x[i], fp[k].y[i]); ys[i] = TY(fp[k].x[i], fp[k].y[i]); }
            fill_poly(c, xs, ys, fp[k].n, fin);
        }
    }
    /* body */
    {
        float xs[10], ys[10];
        for (int i = 0; i < 10; i++) { xs[i] = TX(fm->x[i], fm->y[i]); ys[i] = TY(fm->x[i], fm->y[i]); }
        fill_poly(c, xs, ys, 10, body);
        /* its thickness: the cross-section at the deepest point, widening as
         * the fish turns toward the glass (sin of the yaw angle) */
        float sm = yaw_at(yh, yt, 2), rx = FISH_THICK * f->size * sqrtf(fmaxf(0, 1 - sm * sm));
        if (rx >= 1) fill_ellipse(c, TX(2, 0), TY(2, 0), rx, 9.5f * (H / 10) * f->size, body, 255);
    }
    /* the pattern, in the accent color. A FRY has none yet: its markings
     * come in with its first growth spurt */
    if (f->stage >= STAGE_JUV && f->pattern != PATTERN_SOLID) {   /* SOLID: no markings at all */
        int pat = f->pattern < PATTERN_N ? f->pattern : 0;
        if (pat == PATTERN_BARS) {
            /* on the flank, so they narrow with the turn and fade as the fish
             * comes head-on (seen edge-on, a flank shows nothing) */
            for (int i = -1; i <= 1; i++) {
                float lx = fm->bar_x + i * fm->bar_dx, s = fabsf(yaw_at(yh, yt, lx));
                if (s > 0.25f)
                    fill_ellipse(c, TX(lx, 0), TY(lx, 0), 2 * mark * s, 0.5f * H * mark,
                                 f->accent, (int)(150 * (s - 0.25f) / 0.75f));
            }
        } else {
            /* MASK: the face, the body cut along a curved gill edge just
             * behind the eye (a convex cut of a convex body: four half-planes).
             * EYE BAND: a slanted band through the eye, the body cut between
             * two lines. Neither fades with the turn: the face shows head-on. */
            /* the cut lines, each a -> b: the side kept is the nose's for a
             * line drawn downward, the tail's for one drawn upward */
            float ex = fm->eye_x, cut[4][4]; int nc;
            if (pat == PATTERN_MASK) {
                float gx = ex - 5;
                const float px[5] = { gx, gx - 3, gx - 3.5f, gx - 3, gx }, py[5] = { -1.3f * H, -0.4f * H, 0, 0.4f * H, 1.3f * H };
                for (nc = 0; nc < 4; nc++) { cut[nc][0] = px[nc]; cut[nc][1] = py[nc]; cut[nc][2] = px[nc + 1]; cut[nc][3] = py[nc + 1]; }
            } else {
                /* 5.5 px wide, leaning back, the eye inside it */
                const float back[4]  = { ex - 0.5f, -1.3f * H, ex - 6, 1.3f * H };     /* down: keep the nose side */
                const float front[4] = { ex - 0.5f, 1.3f * H, ex + 5, -1.3f * H };     /* up: keep the tail side */
                memcpy(cut[0], back, sizeof back); memcpy(cut[1], front, sizeof front); nc = 2;
            }
            float ax[16], ay[16], bx[16], by[16]; int n = 10;
            memcpy(ax, fm->x, sizeof fm->x); memcpy(ay, fm->y, sizeof fm->y);
            for (int k = 0; k < nc && n >= 3; k++) {
                n = clip_side(ax, ay, n, cut[k][0], cut[k][1], cut[k][2], cut[k][3], bx, by);
                memcpy(ax, bx, sizeof(float) * n); memcpy(ay, by, sizeof(float) * n);
            }
            if (n >= 3) {
                for (int i = 0; i < n; i++) { float lx = ax[i], ly = ay[i]; ax[i] = TX(lx, ly); ay[i] = TY(lx, ly); }
                fill_poly_a(c, ax, ay, n, f->accent, 235);
            }
        }
    }
    /* eye — closed to a lid line when asleep (resting at night) */
    float ex = fm->eye_x, ey = fm->eye_y;
    if (asleep) {
        fill_ellipse(c, TX(ex, ey), TY(ex, ey), 2.2f, 0.7f, 0x9fb4b8, 200);
    } else {
        fill_ellipse(c, TX(ex, ey), TY(ex, ey), 2.2f, 2.2f, 0xffffff, 255);
        fill_ellipse(c, TX(ex + 0.7f, ey), TY(ex + 0.7f, ey), 1.1f, 1.1f, 0x031015, 255);
    }
#undef TX
#undef TY
#undef BX
#undef BY
}
void draw_fish(ctx_t *c, const tank_t *t, const fish_t *f) {
    if (g_no_animals) return;
    ctx_t cf = *c;                                     /* its window ends at the sand line (FISH_FLOOR_Y): a fin that reaches
                                                          the floor touches it, never shows through it - nothing of a fish is
                                                          ever painted over the sand or a piece's footing (2026-10-24) */
    int h = FISH_FLOOR_Y - cf.oy; if (h < cf.h) cf.h = h;
    bool asleep = t->night && (f->goal.id == GOAL_W_REST || f->pose == LZ_POSE_REST || f->pose == LZ_POSE_HIDDEN);   /* (a lizard lying still asleep: THE WALK's pose) */
    /* and its BODY rests on the sand, never in it: the physics lets a centre
       down to 15 x size off the bottom (tank.c's hard clamp; a pellet lies
       2 px into the sand), where the biggest fish's belly would be 8 px under
       the line and the window would cut it flat. Drawn that much higher, it
       lies on the sand with its mouth at the pellet - smoothly, the lift
       growing as it sinks - and only a fin ever reaches the window's edge. */
    float sink = f->y + 10 * f->size - FISH_FLOOR_Y;
    if (sink > 0) { fish_t g = *f; g.y -= sink; draw_fish_core(&cf, &g, f->beat, asleep, 1.0f, FISH_FLOOR_Y); return; }
    draw_fish_core(&cf, f, f->beat, asleep, 1.0f, FISH_FLOOR_Y);
}
float render_fish_beat(const tank_t *t, int i) { return t->fish[i].beat + t->fish[i].wander; }   /* (draw_fish_core's) */

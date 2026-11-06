/* body.c - THE LIZARD's page art (issue #4 phase 3: moved out of render.c, where it stood behind PT_TERRARIUM).
 *
 * What the keeper page, the shop, the setup and the cards say and show of a lizard: its portrait, its glyph on the strips, the egg it
 * comes from, its morph's words and colour. body.h's descriptor; the body itself (draw_lizard) is lizard.c. */
#include "render_core.h"
#include "body.h"
#include "bodies/lizard/lizard.h"
#include "pt_util.h"
#include <stdio.h>

/* a lizard's portrait (THE TERRARIUM: a card, the keeper page's rows, the setup's colours): side-on, standing, facing right,
   blinking and flicking its tongue now and then on the page's clock - its morph by its body colour (f), its stage, its shed
   and its markings' seed (form, the lizard itself, when there is one). Its whole reach - the tail's tip to the snout's with
   the tongue out (lizard_reach) - is LZ_STILL_K x the box a classic fish of its stage takes at that size (fish_box), centred
   on that box, inside the room's (a classic elder's box x room, as fish_fit's): the keeper page's zones are drawn round the
   fish's box (ms_anim: the GROW gate's cell is 40 px for a fish of 29) */
#define LZ_STILL_K 1.15f
static void lizard_page_mark(void *user, int x0, int x1, int y) {   /* (the page's own coordinates: its box - the keeper page's moving things - and the frame's touched mask) */
    const ctx_t *c = (const ctx_t *)user;
    bb_add(x0, x1, y); touch_span_f(x0 - c->ox, x1 - c->ox, y - c->oy);
}
static void lizard_still(ctx_t *c, const species_t *sp, float x, float y, float size, fish_t *f, const fish_t *form, double clock, float room) {
    if (!sp || sp->plan != PLAN_LIZARD) return;
    const fish_t *who = form ? form : f;
    const morph_t flat = { "", f->color, f->color };            /* a silhouette (the keeper page's not-yet, the empty places: one colour
                                                                   given for all three) is the lizard in that colour, not in its morph's */
    const bool silhouette = f->color == f->fin && f->fin == f->accent;
    lizard_look_t lk = { &sp->geom, silhouette ? &flat : lizard_morph_of(sp, f), &sp->pattern[f->stage & 3], (uint8_t)f->stage, who->uid ? who->uid : f->color };
    float r[4], e[4]; ref_box((stage_t)f->stage, r); ref_box(STAGE_ELDER, e);
    float w = LZ_STILL_K * (r[1] - r[0]) * size;
    if (room > 0 && w > room * (e[1] - e[0]) * size) w = room * (e[1] - e[0]) * size;
    float scale = w / sp->geom.length_px, back, ahead;
    for (int i = 0; i < 4; i++) { lizard_reach(&sp->geom, scale, &back, &ahead); scale *= w / (back + ahead); }   /* (the reach's ends do not
                                                                                                  scale with it: a few steps settle it) */
    lizard_reach(&sp->geom, scale, &back, &ahead);
    const float len = sp->geom.length_px * scale, mid = x + 0.5f * (r[0] + r[1]) * size + 0.5f * (back - ahead);
    lizard_pose_t p = lizard_pose(LZ_POSE_STAND, 1);
    const float a = (float)fmod(clock, 60.0);
    const float bt = fmodf(a, 4.3f), tt = fmodf(a + 1.7f, 6.1f);
    p.blink = bt < 0.18f ? 1 - fabsf(bt / 0.09f - 1) : 0;
    p.tongue = tt < 0.26f ? sinf(3.14159265f * tt / 0.26f) : 0;
    p.fat = who->lz_fat > 0 ? who->lz_fat : 0.6f; p.clock = a;
    p.shed = who->shed == SHED_CLEAN ? SHED_NORMAL : who->shed;
    if (p.shed == SHED_SHEDDING) p.shed_k = clampf(1 - who->shed_s / (sp->shed_window_s > 0 ? sp->shed_window_s : 1), 0, 1);
    lz_canvas_t cv; memset(&cv, 0, sizeof cv);
    cv.fb = c->fb; cv.stride = c->stride; cv.ox = c->ox; cv.oy = c->oy; cv.w = c->w; cv.h = c->h; cv.dim = 1.0f;
    cv.mark = lizard_page_mark; cv.user = c;
    draw_lizard(&cv, &lk, &p, mid, y + 0.07f * len, scale);          /* (its feet a little under the portrait's middle) */
}
/* a tiny gecko facing right, about the fish's box - a long low body, the head ahead of it, a
   tapering tail behind and four short legs */
static void lizard_glyph(ctx_t *c, float cx, float cy, float r, uint32_t rgb) {
    const float s = r * 0.8f;
    fill_ellipse(c, cx, cy, s * 1.25f, s * 0.5f, rgb, 255);                          /* the body */
    fill_ellipse(c, cx + s * 1.45f, cy - s * 0.15f, s * 0.6f, s * 0.42f, rgb, 255);   /* the head */
    float xs[3] = { cx - s * 0.9f, cx - s * 3.1f, cx - s * 0.9f }, ys[3] = { cy - s * 0.4f, cy + s * 0.15f, cy + s * 0.4f };
    fill_poly(c, xs, ys, 3, rgb);                                                     /* the tail */
    const int lw = s > 3 ? 2 : 1, lh = (int)(s * 0.55f + 1.5f);
    for (int k = 0; k < 2; k++) rect_fill(c, (int)(cx + (k ? s * 0.7f : -s * 0.85f)), (int)(cy + s * 0.3f), lw, lh, rgb);   /* the near legs */
}
/* the next arrival on the keeper page (the NEW EGG row, its modal): an upright leathery egg, lit up-left, in
   the fry-to-be's colour - teal once every gate is met, the silhouette's grey before */
static void egg_glyph(ctx_t *c, float cx, float cy, float s, uint32_t rgb) {
    fill_ellipse(c, cx, cy, 6.5f * s, 9.0f * s, rgb, 255);
    fill_ellipse(c, cx - 2.2f * s, cy - 3.6f * s, 2.0f * s, 3.0f * s, 0xffffff, 80);
}
static void lizard_unborn(uint16_t *fb, int stride, float x, float y, uint32_t rgb, double clock) {
    (void)clock;
    ctx_t c = ctx_page(fb, stride); egg_glyph(&c, x, y, 1.0f, rgb);
}
/* a lizard's growth strip wears its morph's body colour (the accent's gene draws nothing on it) */
static uint32_t lizard_strip_rgb(const species_t *sp, const fish_t *f) {
    const morph_t *m = lizard_morph_of(sp, f);
    if (m) return m->body;
    return f->accent;
}
/* a lizard's card's second line: its morph */
static void lizard_genes_line(const species_t *sp, const fish_t *f, char *out, size_t n) {
    const int m = lizard_morph_index(sp, f);
    snprintf(out, n, "%s MORPH", m >= 0 ? sp->morphs[m].name : "?");
}
/* an offered young one in a word: the lizard's morph */
static const char *lizard_offer_word(const species_t *sp, const fry_offer_t *o) {
    fish_t f; memset(&f, 0, sizeof f); f.color = o->body;
    const morph_t *m = lizard_morph_of(sp, &f);
    if (m) return m->name;
    return SHAPE_NAMES[o->shape % SHAPE_N];
}

const body_ops_t lizard_body = {
    .plan = PLAN_LIZARD, .name = "lizard",
    .stage_words = { "HATCHLING", "JUVENILE", "ADULT", "ELDER" },
    .still = lizard_still, .glyph = lizard_glyph, .unborn = lizard_unborn,
    .strip_rgb = lizard_strip_rgb, .genes_line = lizard_genes_line, .offer_word = lizard_offer_word,
};

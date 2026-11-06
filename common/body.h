/* body.h - the body types (issue #4 phase 3): what the renderer asks of an animal's body, per species, never per build.
 *
 * A body type (common/bodies/<body>/: the fish, the lizard) draws one kind of animal and speaks of it: its portrait on a page, its
 * glyph on a strip, the words of its stages and its form. The pages reach it through the species' body plan (species.h body_plan_t):
 *
 *     body_of(sp)        the module of a species;      tank_body(t, i)   the module of animal i of a tank (today the tank's one
 *                                                        species; the format keeps room for a species per animal)
 *
 * The bodies a world draws are its own list (world_view.h `bodies`): the aquarium's fish, the terrarium's lizards; a world that
 * holds both is a list of both. A world's SCENE draws its animals with the body module directly (bodies/fish/fish.h draw_fish,
 * bodies/lizard/lizard.h draw_lizard): it is the scene that knows what stands where, what lights it and what hides it. */
#ifndef BODY_H
#define BODY_H
#include "render_core.h"
#include "species.h"
#include "progression.h"       /* fry_offer_t */

typedef struct body_ops_s {
    body_plan_t plan;                                /* species.h: the plan this module draws */
    const char *name;                                /* "fish" / "lizard" */
    const char *stage_words[4];                      /* its stages in a keeper's words: a newborn, a juvenile, an adult, an elder */

    /* on a page: a portrait side-on, facing right, in the colours of f (f->stage is its stage; form, when there is one, the animal itself:
       its genes and markings' seed), in `room` (FISH_ROOM_*: the room a classic fish of the stage has; 0: as it is) on the page's clock */
    void (*still)(ctx_t *c, const species_t *sp, float x, float y, float size, fish_t *f, const fish_t *form, double clock, float room);
    /* ... a tiny one, facing right, for the growth and population strips: about a fish's box */
    void (*glyph)(ctx_t *c, float cx, float cy, float r, uint32_t rgb);
    /* ... the next arrival on the keeper page before it is born (an egg, a fry's silhouette), at its row's place, in one colour */
    void (*unborn)(uint16_t *fb, int stride, float x, float y, uint32_t rgb, double clock);
    /* ... the colour its growth strip wears when grown (a lizard its morph's body colour, a fish its accent) */
    uint32_t (*strip_rgb)(const species_t *sp, const fish_t *f);
    /* ... the second line of its card: what it is, or will grow into (a morph; a shape, fins and pattern) */
    void (*genes_line)(const species_t *sp, const fish_t *f, char *out, size_t n);
    /* ... an offered young one in a word (the shop's FRY row): a morph's name, a body shape's */
    const char *(*offer_word)(const species_t *sp, const fry_offer_t *o);
} body_ops_t;

const body_ops_t *body_of(const species_t *sp);
const body_ops_t *tank_body(const tank_t *t, int i);

#endif

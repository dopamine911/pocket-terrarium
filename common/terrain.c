/* terrain.c - THE TERRAIN (terrain.h): the glass at run time, a habitat's ground laid out on it, the paths' arithmetic,
 * the route and the rules */
#include "terrain.h"
#include "tank.h"                       /* TANK_*: this build's glass (glass_here) */
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the glass ---- */
static const glass_t GLASS_BOARD[GLASS_BOARDS] = {
    { "rect",   448, 368, GLASS_RECT,      0.0f, 368 },   /* the 1.8 */
    { "round",  466, 466, GLASS_ROUND,   233.0f, 396 },   /* the 1.75C pendant: a bowl */
    { "watch",  410, 502, GLASS_CORNERS, 100.0f, 502 },   /* the 2.06 watch, portrait */
    { "square", 480, 480, GLASS_CORNERS,  40.0f, 480 },   /* the 2.16 square */
};
glass_t glass_of(int k) { return GLASS_BOARD[k < 0 ? 0 : k >= GLASS_BOARDS ? GLASS_BOARDS - 1 : k]; }
glass_t glass_here(void) {
#if defined(TANK_ROUND)
    glass_t g = { "here", TANK_W, TANK_H, GLASS_ROUND, TANK_RAD, TANK_BOT };
#elif defined(TANK_CORNER_R)
    glass_t g = { "here", TANK_W, TANK_H, GLASS_CORNERS, TANK_CORNER_R, TANK_BOT };
#else
    glass_t g = { "here", TANK_W, TANK_H, GLASS_RECT, 0.0f, TANK_BOT };
#endif
    return g;
}
static float corner_inset(float r, float v, float len) {         /* tank.c corner_inset */
    float d = v < r ? r - v : v > len - r ? v - (len - r) : 0;
    if (d <= 0) return 0;
    return d >= r ? r : r - sqrtf(r * r - d * d);
}
static float bowl_in(float r, float v) { float d = v - r, r2 = r * r - d * d; return r - (r2 > 0 ? sqrtf(r2) : 0); }
float glass_x0(const glass_t *g, float y) {
    if (g->kind == GLASS_ROUND) return bowl_in(g->r, y);
    if (g->kind == GLASS_CORNERS) return corner_inset(g->r, y, (float)g->h);
    return 0;
}
float glass_x1(const glass_t *g, float y) { return (float)g->w - glass_x0(g, y); }
float glass_top(const glass_t *g, float x) {
    if (g->kind == GLASS_ROUND) return bowl_in(g->r, x);
    if (g->kind == GLASS_CORNERS) return corner_inset(g->r, x, (float)g->w);
    return 0;
}
float glass_place_y(const glass_t *g) { return (float)g->bot - TERRAIN_FLOOR_DY; }
float glass_floor_x(const glass_t *g, float u) {                /* tank.c floor_span: the glass at PLACE_Y, TERRAIN_MARGIN in */
    const float y = glass_place_y(g), x0 = glass_x0(g, y) + TERRAIN_MARGIN, x1 = glass_x1(g, y) - TERRAIN_MARGIN;
    return x0 + (x1 - x0) * u;
}

/* ---- laying it out ---- */
static float map_y(const glass_t *g, float v) { return glass_place_y(g) * (1 - v); }
static float map_x(const glass_t *g, float u, float y) {
    const float x0 = glass_x0(g, y) + TERRAIN_MARGIN, x1 = glass_x1(g, y) - TERRAIN_MARGIN;
    return x0 + (x1 - x0) * u;
}
void terrain_point(const terrain_t *tr, const habitat_t *h, int surf, float u, float *x, float *y) {
    if (surf <= 0 || surf > tr->n_rocks) { *x = glass_floor_x(&tr->glass, u); *y = glass_place_y(&tr->glass); return; }
    const rock_t *r = &h->rocks[surf - 1];
    const terrain_path_t *p = &tr->path[surf];
    int i = 0;
    while (i + 2 < r->n && u > r->u[i + 1]) i++;                 /* the segment holding u (its last beyond the end) */
    const float k = r->u[i + 1] > r->u[i] ? (u - r->u[i]) / (r->u[i + 1] - r->u[i]) : 0;
    *x = p->x[i] + (p->x[i + 1] - p->x[i]) * k;
    *y = p->y[i] + (p->y[i + 1] - p->y[i]) * k;
}
void terrain_lay(const habitat_t *h, const glass_t *g, terrain_t *o) {
    memset(o, 0, sizeof *o);
    o->glass = *g;
    const float py = glass_place_y(g);
    terrain_path_t *f = &o->path[0];                             /* the floor, glass to glass */
    f->kind = PATH_FLOOR; f->n = 2;
    f->x[0] = glass_x0(g, py); f->x[1] = glass_x1(g, py); f->y[0] = f->y[1] = py;
    o->n_rocks = h->n_rocks > HABITAT_ROCKS_MAX ? HABITAT_ROCKS_MAX : h->n_rocks;
    o->n_paths = 1;
    for (int k = 0; k < o->n_rocks; k++) {
        const rock_t *r = &h->rocks[k];
        terrain_path_t *p = &o->path[o->n_paths++];
        p->kind = PATH_ROCK; p->n = r->n < 2 ? 2 : r->n > ROCK_PTS_MAX ? ROCK_PTS_MAX : r->n;
        for (int i = 0; i < p->n; i++) { p->y[i] = map_y(g, r->v[i]); p->x[i] = map_x(g, r->u[i], p->y[i]); }
    }
    const int n_ramps = h->n_ramps > HABITAT_RAMPS_MAX ? HABITAT_RAMPS_MAX : h->n_ramps;
    for (int k = 0; k < n_ramps; k++) {
        const ramp_t *m = &h->ramps[k];
        float ax, ay, bx, by;
        terrain_point(o, h, m->lo, m->u_lo, &ax, &ay);
        terrain_point(o, h, m->hi, m->u_hi, &bx, &by);
        const int id = o->n_paths++;
        terrain_path_t *p = &o->path[id];
        p->kind = PATH_RAMP; p->n = 2;
        const bool foot_left = ax <= bx;                         /* (left to right, whichever end is the foot) */
        p->x[0] = foot_left ? ax : bx; p->y[0] = foot_left ? ay : by;
        p->x[1] = foot_left ? bx : ax; p->y[1] = foot_left ? by : ay;
        terrain_link_t *l = &o->link[o->n_links++];
        l->a = (uint8_t)id; l->b = (uint8_t)(m->lo <= o->n_rocks ? m->lo : 0); l->x = ax; l->y = ay;
        l = &o->link[o->n_links++];
        l->a = (uint8_t)id; l->b = (uint8_t)(m->hi <= o->n_rocks ? m->hi : 0); l->x = bx; l->y = by;
    }
    o->bask_path = (int8_t)(h->bask_surf <= o->n_rocks ? h->bask_surf : 0);
    terrain_point(o, h, o->bask_path, h->bask_u, &o->bask_x, &o->bask_y);
    for (int k = 0; k < o->n_paths; k++) {                       /* each segment's cost a px across, either way */
        terrain_path_t *p = &o->path[k];
        for (int i = 0; i + 1 < p->n; i++) {
            const float dx = p->x[i + 1] - p->x[i], dy = p->y[i + 1] - p->y[i], len = sqrtf(dx * dx + dy * dy);
            if (!(dx > 0)) { p->cost_r[i] = p->cost_l[i] = 0; continue; }
            const float rise = -dy / len;                            /* sin of its climb walked rightward (screen y runs down) */
            p->cost_r[i] = len / dx / (rise > 0 ? 1 - TERRAIN_UP_SLOW * rise : 1 + TERRAIN_DOWN_SLOW * rise);
            p->cost_l[i] = len / dx / (rise < 0 ? 1 + TERRAIN_UP_SLOW * rise : 1 - TERRAIN_DOWN_SLOW * rise);
        }
    }
}

/* ---- along a path ---- */
static int seg_at(const terrain_path_t *p, float x, int dir) {   /* the segment under x (at a point: the one toward dir) */
    int i = 0;
    while (i + 2 < p->n && (dir > 0 ? x >= p->x[i + 1] : x > p->x[i + 1])) i++;
    return i;
}
float terrain_y(const terrain_path_t *p, float x) {
    if (x <= p->x[0]) return p->y[0];
    if (x >= p->x[p->n - 1]) return p->y[p->n - 1];
    const int i = seg_at(p, x, 1);
    const float dx = p->x[i + 1] - p->x[i];
    return dx > 0 ? p->y[i] + (p->y[i + 1] - p->y[i]) * (x - p->x[i]) / dx : p->y[i];
}
float terrain_tilt(const terrain_path_t *p, float x, int dir) {
    const int i = seg_at(p, x, dir);
    return atan2f(p->y[i] - p->y[i + 1], p->x[i + 1] - p->x[i]);   /* (screen y runs down: rising to the right is +) */
}
float terrain_pace(float climb) {
    const float s = sinf(climb);
    return s > 0 ? 1 - TERRAIN_UP_SLOW * s : 1 + TERRAIN_DOWN_SLOW * s;
}
bool terrain_on(const terrain_path_t *p, float x, float y, float tol) {
    return x >= p->x[0] - tol && x <= p->x[p->n - 1] + tol && fabsf(terrain_y(p, x) - y) <= tol;
}
int terrain_at(const terrain_t *tr, float x, float y) {
    int best = 0; float bd = 1e9f;
    for (int k = 0; k < tr->n_paths; k++) {
        const terrain_path_t *p = &tr->path[k];
        if (x < p->x[0] - 0.5f || x > p->x[p->n - 1] + 0.5f) continue;
        const float d = fabsf(terrain_y(p, x) - y);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}
/* walking from xa to xb along p: its length over the pace on each slope (seconds at 1 px/s) */
static float path_cost(const terrain_path_t *p, float xa, float xb) {
    const float lo = fminf(xa, xb), hi = fmaxf(xa, xb);
    const float *k = xb >= xa ? p->cost_r : p->cost_l;
    float c = 0;
    for (int i = 0; i + 1 < p->n; i++) {
        const float s0 = fmaxf(lo, p->x[i]), s1 = fminf(hi, p->x[i + 1]);
        if (s1 > s0) c += (s1 - s0) * k[i];
    }
    return c;
}

/* ---- the route: Dijkstra over the links' sides - node 2k is link k's point standing on its path a, 2k + 1 on its b ---- */
#define ROUTE_NODES (2 * TERRAIN_LINKS_MAX)
static int node_path(const terrain_t *tr, int i) { return (i & 1) ? tr->link[i >> 1].b : tr->link[i >> 1].a; }
bool terrain_route(const terrain_t *tr, int from, float fx, int to, float tx, terrain_hop_t *hop) {
    const float INF = 1e30f;
    const int nn = 2 * tr->n_links;
    float dist[ROUTE_NODES]; int8_t prev[ROUTE_NODES]; bool done[ROUTE_NODES];
    for (int i = 0; i < nn; i++) {
        dist[i] = node_path(tr, i) == from ? path_cost(&tr->path[from], fx, tr->link[i >> 1].x) : INF;
        prev[i] = -1; done[i] = false;
    }
    for (;;) {
        int u = -1;
        for (int i = 0; i < nn; i++) if (!done[i] && dist[i] < INF && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0) break;
        done[u] = true;
        const int pu = node_path(tr, u);
        const float xu = tr->link[u >> 1].x;
        if (dist[u] < dist[u ^ 1]) { dist[u ^ 1] = dist[u]; prev[u ^ 1] = (int8_t)u; }   /* step across the link */
        for (int j = 0; j < nn; j++) {                                                    /* walk on along this path */
            if (done[j] || (j >> 1) == (u >> 1) || node_path(tr, j) != pu) continue;
            const float d = dist[u] + path_cost(&tr->path[pu], xu, tr->link[j >> 1].x);
            if (d < dist[j]) { dist[j] = d; prev[j] = (int8_t)u; }
        }
    }
    float best = from == to ? path_cost(&tr->path[from], fx, tx) : INF;
    int last = -1;
    for (int i = 0; i < nn; i++) {
        if (node_path(tr, i) != to || dist[i] >= INF) continue;
        const float d = dist[i] + path_cost(&tr->path[to], tr->link[i >> 1].x, tx);
        if (d < best - 0.01f) { best = d; last = i; }
    }
    if (best >= INF) return false;
    hop->cost = best; hop->via = -1; hop->x = tx; hop->entry = fx;
    if (last < 0) return true;                                   /* along its own path, straight there */
    hop->entry = tr->link[last >> 1].x;
    int chain[ROUTE_NODES + 1], n = 0;
    for (int i = last; i >= 0 && n <= ROUTE_NODES; i = prev[i]) chain[n++] = i;
    for (int k = n - 1; k >= 0; k--)                             /* from the start: the first step onto another path */
        if (node_path(tr, chain[k]) != from) { hop->x = tr->link[chain[k] >> 1].x; hop->via = node_path(tr, chain[k]); return true; }
    return true;                                                 /* (back onto its own path: the destination is on it) */
}

/* ---- the air ---- */
static float air_floor_c(const terrain_t *tr, const habitat_t *h, float x) {   /* the floor's gradient at x */
    const float x0 = glass_floor_x(&tr->glass, 0), x1 = glass_floor_x(&tr->glass, 1);
    float u = (x - x0) / (x1 - x0); u = u < 0 ? 0 : u > 1 ? 1 : u;
    return h->floor_warm_c + (h->floor_cool_c - h->floor_warm_c) * u;
}
float terrain_air_open_c(const terrain_t *tr, const habitat_t *h, float x, float y) {
    const float py = glass_place_y(&tr->glass);
    float v = (py - y) / py; v = v < 0 ? 0 : v > 1 ? 1 : v;
    return air_floor_c(tr, h, x) + h->rise_c * v;
}
float terrain_air_c(const terrain_t *tr, const habitat_t *h, float x, float y, bool lit) {
    if (!lit) return air_floor_c(tr, h, x) - h->night_drop_c;            /* the lamp out, the night's cool */
    float c = terrain_air_open_c(tr, h, x, y);
    const float dx = x - tr->bask_x, dy = y - tr->bask_y, d2 = dx * dx + dy * dy, r = h->bask_r_px;
    if (d2 < r * r) c += (h->bask_c - c) * (1 - d2 / (r * r));          /* the lamp's pool: bask_c in the middle, the air's own at its edge */
    return c;
}

/* ---- the rules on one glass ---- */
/* two paths over one x closer than TERRAIN_HEADROOM_PX are fine where they part from a point both pass through: a ramp
   and the surface it lands on (they part at its end), or two near a point they share (a switchback's corner) */
static bool both_pass(const terrain_t *tr, int a, int b, float x) {
    for (int k = 0; k < tr->n_links; k++) {
        const terrain_link_t *l = &tr->link[k];
        if ((l->a == a && l->b == b) || (l->a == b && l->b == a)) return true;
        if (fabsf(l->x - x) > TERRAIN_JUNCTION_PX) continue;
        if (terrain_on(&tr->path[a], l->x, l->y, 0.75f) && terrain_on(&tr->path[b], l->x, l->y, 0.75f)) return true;
    }
    return false;
}
bool terrain_check(const habitat_t *h, const glass_t *g, char *why, size_t n) {
    #define REFUSE(...) do { if (why && n) { int w_ = snprintf(why, n, "on the %s: ", g->name); if (w_ > 0 && (size_t)w_ < n) snprintf(why + w_, n - (size_t)w_, __VA_ARGS__); } return false; } while (0)
    static const char *const KIND[3] = { "the floor", "rock", "ramp" };
    terrain_t tr; terrain_lay(h, g, &tr);
    const float py = glass_place_y(g), slope_max = tanf(TERRAIN_SLOPE_MAX * 3.14159265f / 180.0f);
    for (int k = 1; k < tr.n_paths; k++) {
        const terrain_path_t *p = &tr.path[k];
        const int no = p->kind == PATH_ROCK ? k : k - tr.n_rocks;
        for (int i = 0; i + 1 < p->n; i++) {                     /* walkable: no slope past the gecko's, no step */
            const float dx = p->x[i + 1] - p->x[i], dy = fabsf(p->y[i + 1] - p->y[i]);
            if (!(dx > 0) || dy > dx * slope_max)
                REFUSE("%s %d climbs %.0f px over %.0f px (%.0f deg), steeper than %.0f deg: a step, not walkable", KIND[p->kind], no, dy, dx,
                       atan2f(dy, dx) * 57.29578f, TERRAIN_SLOPE_MAX);
        }
        for (float x = p->x[0]; x <= p->x[p->n - 1] + 0.01f; x += 2) {   /* inside the glass, a lizard's height under it */
            const float xx = fminf(x, p->x[p->n - 1]), y = terrain_y(p, xx);
            if (xx < glass_x0(g, y) + TERRAIN_MARGIN - 0.5f || xx > glass_x1(g, y) - TERRAIN_MARGIN + 0.5f)
                REFUSE("%s %d at (%.0f, %.0f) is within %.0f px of the side glass", KIND[p->kind], no, xx, y, TERRAIN_MARGIN);
            if (y - TERRAIN_HEAD_PX < glass_top(g, xx)) REFUSE("%s %d at (%.0f, %.0f) has no room for a lizard under the glass", KIND[p->kind], no, xx, y);
            if (p->kind == PATH_ROCK && y > py - TERRAIN_LIFT_PX)
                REFUSE("rock %d at x %.0f is %.0f px over the floor, under %.0f: a raised rock leaves the floor open", no, xx, py - y, TERRAIN_LIFT_PX);
        }
    }
    for (int k = 0; k < tr.n_links; k++) {                       /* a ramp's ends well in: every lizard reaches them */
        const terrain_link_t *l = &tr.link[k];
        if (l->x < glass_x0(g, l->y) + TERRAIN_LINK_IN_PX - 0.5f || l->x > glass_x1(g, l->y) - TERRAIN_LINK_IN_PX + 0.5f)
            REFUSE("ramp %d ends at (%.0f, %.0f), within %.0f px of the side glass: a big lizard could not reach it", k / 2 + 1, l->x, l->y, TERRAIN_LINK_IN_PX);
    }
    /* every path reachable from the floor */
    bool seen[TERRAIN_PATHS_MAX] = { true };
    for (bool grew = true; grew; ) {
        grew = false;
        for (int k = 0; k < tr.n_links; k++) {
            const terrain_link_t *l = &tr.link[k];
            if (seen[l->a] != seen[l->b]) { seen[l->a] = seen[l->b] = true; grew = true; }
        }
    }
    for (int k = 1; k < tr.n_paths; k++)
        if (!seen[k]) REFUSE("%s %d is out of reach: no ramp joins it to the floor", KIND[tr.path[k].kind], tr.path[k].kind == PATH_ROCK ? k : k - tr.n_rocks);
    /* headroom: two paths over one x */
    for (int a = 0; a < tr.n_paths; a++)
        for (int b = a + 1; b < tr.n_paths; b++) {
            const terrain_path_t *p = &tr.path[a], *q = &tr.path[b];
            const float x0 = fmaxf(p->x[0], q->x[0]), x1 = fminf(p->x[p->n - 1], q->x[q->n - 1]);
            for (float x = x0; x <= x1; x += 2) {
                const float gap = fabsf(terrain_y(p, x) - terrain_y(q, x));
                if (gap < TERRAIN_HEADROOM_PX && !both_pass(&tr, a, b, x))
                    REFUSE("paths %d and %d are %.0f px apart over x %.0f: under %.0f, no room for a lizard between", a, b, gap, x, TERRAIN_HEADROOM_PX);
            }
        }
    /* the lamp: the basking spot is the highest ground under it, with room for it overhead */
    if (h->bask_r_px > 0) {
        if (tr.bask_y - glass_top(g, tr.bask_x) < TERRAIN_LAMP_PX)
            REFUSE("the basking spot (%.0f, %.0f) is %.0f px under the glass: no room for the lamp (%.0f)", tr.bask_x, tr.bask_y, tr.bask_y - glass_top(g, tr.bask_x), TERRAIN_LAMP_PX);
        for (int k = 0; k < tr.n_paths; k++) {
            const terrain_path_t *p = &tr.path[k];
            for (float x = fmaxf(p->x[0], tr.bask_x - h->bask_r_px); x <= fminf(p->x[p->n - 1], tr.bask_x + h->bask_r_px); x += 2)
                if (terrain_y(p, x) < tr.bask_y - 0.5f)
                    REFUSE("%s %d at (%.0f, %.0f) stands over the basking spot (%.0f, %.0f) under the lamp", KIND[p->kind], k, x, terrain_y(p, x), tr.bask_x, tr.bask_y);
        }
    }
    /* the hides show: no rock or ramp low over one */
    for (int j = 0; j < h->n_hides; j++) {
        const float hx = glass_floor_x(g, h->hides[j].u);
        for (int k = 1; k < tr.n_paths; k++) {
            const terrain_path_t *p = &tr.path[k];
            for (float x = fmaxf(p->x[0], hx - TERRAIN_HIDE_HW); x <= fminf(p->x[p->n - 1], hx + TERRAIN_HIDE_HW); x += 2)
                if (py - terrain_y(p, x) < TERRAIN_HIDE_CLEAR_PX)
                    REFUSE("%s %d runs %.0f px over the %s hide at x %.0f: under %.0f, it hides the hide", KIND[p->kind], p->kind == PATH_ROCK ? k : k - tr.n_rocks,
                           py - terrain_y(p, x), HIDE_KIND_NAMES[h->hides[j].kind < HIDE_KINDS ? h->hides[j].kind : 0], x, TERRAIN_HIDE_CLEAR_PX);
        }
    }
    return true;
    #undef REFUSE
}

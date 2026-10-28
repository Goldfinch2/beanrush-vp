/* course.c — the courses: their parts, where each is at any time, what a
 * bean stands on, bumps into or is hit by, and the races a show draws from.
 */
#include "beanrush.h"

#include <math.h>
#include <string.h>

#define HALF_W   6.0f           /* the course is 12 m wide */
#define THICK    1.0f           /* platforms' thickness */

float rnd(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return (float)(*state >> 8) / 16777216.0f;
}

/* ---- where things are ------------------------------------------------------- */

vp_vec3 part_pos(const Part *p, float t)
{
    if (p->type == P_MOVER)
        return vp_v3_add(p->c, vp_v3_scale(p->axis, sinf(p->w * t + p->ph)));
    return p->c;
}

vp_vec3 part_vel(const Part *p, float t)
{
    if (p->type == P_MOVER)
        return vp_v3_scale(p->axis, p->w * cosf(p->w * t + p->ph));
    if (p->type == P_CONVEYOR)
        return vp_v3(0, 0, p->belt);
    return vp_v3(0, 0, 0);
}

float spinner_angle(const Part *p, float t)
{
    return p->w * t + p->ph;
}

float pendulum_angle(const Part *p, float t)
{
    return p->h.z * sinf(p->w * t + p->ph);
}

/* Balls leave the far end every (run + gap) / speed seconds; one rolls
 * toward the start and drops off the near end. */
int roller_ball(const Part *p, float t, vp_vec3 *center, float *run)
{
    const float cycle = p->h.z + 10.0f;
    float d = fmodf(p->w * t + p->ph, cycle);
    if (d < 0) d += cycle;
    *run = d;
    float r = p->h.x;
    if (d > p->h.z + r * 2) return 0;               /* between balls */
    float z = p->c.z - d;
    float y = p->c.y + r;
    if (d > p->h.z) {                               /* off the end: falling */
        float s = (d - p->h.z) / p->w;
        y -= 0.5f * GRAVITY * s * s;
    }
    *center = vp_v3(p->c.x, y, z);
    return 1;
}

/* ---- standing, bumping, being hit ------------------------------------------- */

static int box_like(const Part *p)
{
    return p->type == P_BOX || p->type == P_MOVER || p->type == P_CONVEYOR || p->type == P_TILE;
}

ExtraFloor g_extra_floor;

int course_floor(const Course *c, float x, float z, float y, float t, Support *s)
{
    const float margin = 0.12f;     /* a bean's middle just past the edge still stands */
    int found = 0;
    s->part = -1;
    s->body = NULL;
    s->top = -1e9f;
    s->carry = vp_v3(0, 0, 0);
    s->normal = vp_v3(0, 1, 0);
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (!p->floor || !box_like(p)) continue;
        if (p->type == P_TILE && props_part_gone(i)) continue;     /* dropped away */
        vp_vec3 q = part_pos(p, t);
        if (x < q.x - p->h.x - margin || x > q.x + p->h.x + margin) continue;
        if (z < q.z - p->h.z - margin || z > q.z + p->h.z + margin) continue;
        float top = q.y + p->h.y;
        if (top > y + STEP_UP) continue;            /* a wall, not a floor */
        if (top > s->top) {
            s->top = top;
            s->part = i;
            s->carry = part_vel(p, t);
            found = 1;
        }
    }
    /* a prop higher than the course there (a seesaw over the void) */
    Support e;
    if (g_extra_floor && g_extra_floor(x, z, y, &e) && e.top <= y + STEP_UP && e.top > s->top) {
        *s = e;
        found = 1;
    }
    return found;
}

/* The nearest point to q on the bean's middle line (feet + R to top - R). */
static vp_vec3 bean_axis_point(vp_vec3 pos, float qy)
{
    float y = fmaxf(pos.y + BEAN_R, fminf(qy, pos.y + BEAN_H - BEAN_R));
    return vp_v3(pos.x, y, pos.z);
}

int course_hazard(const Course *c, vp_vec3 pos, float t, vp_vec3 *knock)
{
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (p->type == P_SPINNER) {
            /* the bar: a band 0.45 m thick at h.y above the post's foot */
            float by = p->c.y + p->h.y;
            if (pos.y > by + 0.22f || pos.y + BEAN_H < by - 0.22f) continue;
            float a = spinner_angle(p, t);
            float ux = cosf(a), uz = sinf(a);
            float rx = pos.x - p->c.x, rz = pos.z - p->c.z;
            float along = rx * ux + rz * uz;
            if (along > p->h.x) along = p->h.x;
            if (along < -p->h.x) along = -p->h.x;
            float dx = rx - along * ux, dz = rz - along * uz;
            if (dx * dx + dz * dz > (BEAN_R + 0.2f) * (BEAN_R + 0.2f)) continue;
            /* the bar's own movement there: w x r, across the bar */
            float sx = -uz * p->w * along, sz = ux * p->w * along;
            float sp = sqrtf(sx * sx + sz * sz);
            if (sp < 0.5f) { sx = dx; sz = dz; sp = sqrtf(sx * sx + sz * sz) + 1e-6f; }
            *knock = vp_v3(sx / sp * 10.0f, 6.0f, sz / sp * 10.0f);
            return 1;
        }
        if (p->type == P_PENDULUM) {
            float a = pendulum_angle(p, t);
            vp_vec3 ball = vp_v3(p->c.x + sinf(a) * p->h.y, p->c.y - cosf(a) * p->h.y, p->c.z);
            vp_vec3 q = bean_axis_point(pos, ball.y);
            vp_vec3 d = vp_v3_sub(q, ball);
            float rr = p->h.x + BEAN_R;
            if (vp_v3_dot(d, d) > rr * rr) continue;
            float swing = p->h.z * p->w * cosf(p->w * t + p->ph);   /* its way across */
            float side = swing >= 0 ? 1.0f : -1.0f;
            *knock = vp_v3(side * 12.0f, 6.5f, d.z * 2.0f);
            return 1;
        }
        if (p->type == P_ROLLER) {
            vp_vec3 ball;
            float run;
            if (!roller_ball(p, t, &ball, &run)) continue;
            vp_vec3 q = bean_axis_point(pos, ball.y);
            vp_vec3 d = vp_v3_sub(q, ball);
            float rr = p->h.x + BEAN_R;
            if (vp_v3_dot(d, d) > rr * rr) continue;
            *knock = vp_v3(d.x * 4.0f, 7.0f, -p->w - 5.0f);
            return 1;
        }
    }
    return 0;
}

/* ---- building -------------------------------------------------------------------- */

typedef struct {
    Course  *c;
    float    z;             /* where the next section starts */
    unsigned rng;
} Builder;

static Part *add(Builder *b, PartType type, vp_vec3 c, vp_vec3 h, uint8_t r, uint8_t g, uint8_t bl)
{
    Course *co = b->c;
    if (co->nparts >= MAX_PARTS) return &co->parts[MAX_PARTS - 1];
    Part *p = &co->parts[co->nparts++];
    memset(p, 0, sizeof *p);
    p->type = type;
    p->c = c;
    p->h = h;
    p->floor = (type == P_BOX || type == P_MOVER || type == P_CONVEYOR || type == P_TILE);
    p->solid = p->floor;
    p->r = r; p->g = g; p->b = bl;
    return p;
}

/* A platform from z0 to z1, its top at y. */
static Part *slab(Builder *b, float x0, float x1, float z0, float z1, float y)
{
    return add(b, P_BOX, vp_v3((x0 + x1) / 2, y - THICK / 2, (z0 + z1) / 2),
               vp_v3((x1 - x0) / 2, THICK / 2, (z1 - z0) / 2), 0, 150, 255);
}

static void checkpoint(Builder *b, float x0, float x1, float y)
{
    Course *co = b->c;
    if (co->nchecks >= MAX_CHECKS) return;
    Checkpoint *k = &co->checks[co->nchecks++];
    k->z = b->z + 1.5f;
    k->x0 = x0; k->x1 = x1; k->y = y;
}

static void sec_start(Builder *b)
{
    slab(b, -7, 7, b->z, b->z + 10, 0);
    checkpoint(b, -5, 5, 0);
    b->z += 10;
}

static void sec_flat(Builder *b, float len)
{
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    b->z += len;
}

/* Bars sweeping the course, low: jump them. */
static void sec_spinners(Builder *b, int n, float speed)
{
    float len = 6.0f + n * 8.0f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    for (int i = 0; i < n; i++) {
        float z = b->z + 7.0f + i * 8.0f;
        add(b, P_BOX, vp_v3(0, 0.8f, z), vp_v3(0.3f, 0.8f, 0.3f), 255, 140, 0);   /* the post */
        Part *s = add(b, P_SPINNER, vp_v3(0, 0, z), vp_v3(HALF_W + 0.3f, 0.55f, 0), 255, 60, 40);
        s->w = (i & 1) ? -speed : speed;
        s->ph = rnd(&b->rng) * 6.2832f;
    }
    b->z += len;
}

/* A gap crossed on platforms sliding across it. */
static void sec_movers(Builder *b, int n, float speed, float amp)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2;
    for (int i = 0; i < n; i++) {
        float z = b->z + 1.2f + 1.6f + i * 4.4f;     /* 3.2 m platforms, 1.2 m gaps */
        Part *m = add(b, P_MOVER, vp_v3(0, -THICK / 2, z), vp_v3(1.8f, THICK / 2, 1.6f), 0, 255, 120);
        m->axis = vp_v3(amp, 0, 0);
        m->w = speed * (0.8f + 0.4f * rnd(&b->rng));
        m->ph = rnd(&b->rng) * 6.2832f;
    }
    b->z += n * 4.4f + 1.2f;
}

/* Wrecking balls swinging across. */
static void sec_pendulums(Builder *b, int n, float speed)
{
    float len = 4.0f + n * 6.0f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    for (int i = 0; i < n; i++) {
        float z = b->z + 4.0f + i * 6.0f;
        Part *p = add(b, P_PENDULUM, vp_v3(0, 8.2f, z), vp_v3(1.1f, 7.0f, 0.95f), 255, 40, 200);
        p->w = speed * (0.9f + 0.2f * rnd(&b->rng));
        p->ph = rnd(&b->rng) * 6.2832f;
        /* the frame it hangs from (drawn; its feet stand off the course) */
        Part *l = add(b, P_BOX, vp_v3(-HALF_W - 0.8f, 4.1f, z), vp_v3(0.25f, 4.1f, 0.25f), 120, 120, 160);
        Part *r = add(b, P_BOX, vp_v3(HALF_W + 0.8f, 4.1f, z), vp_v3(0.25f, 4.1f, 0.25f), 120, 120, 160);
        Part *t = add(b, P_BOX, vp_v3(0, 8.45f, z), vp_v3(HALF_W + 1.05f, 0.25f, 0.25f), 120, 120, 160);
        l->floor = r->floor = t->floor = 0;
        l->solid = r->solid = t->solid = 0;
    }
    b->z += len;
}

/* Blocks sliding across, shoving beans off the sides. */
static void sec_pushers(Builder *b, int n, float speed)
{
    float len = 4.0f + n * 5.0f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    for (int i = 0; i < n; i++) {
        float z = b->z + 4.0f + i * 5.0f;
        Part *m = add(b, P_MOVER, vp_v3(0, 0.8f, z), vp_v3(1.4f, 0.8f, 0.7f), 255, 220, 0);
        m->floor = 0;
        m->axis = vp_v3(HALF_W - 1.0f, 0, 0);
        m->w = speed * (0.8f + 0.4f * rnd(&b->rng));
        m->ph = rnd(&b->rng) * 6.2832f;
    }
    b->z += len;
}

/* Big balls rolling down lanes toward the start. */
static void sec_rollers(Builder *b, float len, float speed)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    static const float lanes[3] = { -3.6f, 0.0f, 3.6f };
    for (int i = 0; i < 3; i++) {
        Part *p = add(b, P_ROLLER, vp_v3(lanes[i], 0, b->z + len), vp_v3(1.35f, 0, len), 255, 255, 255);
        p->w = speed;
        p->ph = (len + 10.0f) * (i / 3.0f) + rnd(&b->rng) * 4.0f;
    }
    b->z += len;
}

/* A belt running back toward the start. */
static void sec_conveyor(Builder *b, float len, float speed)
{
    checkpoint(b, -4, 4, 0);
    Part *p = add(b, P_CONVEYOR, vp_v3(0, -THICK / 2, b->z + len / 2),
                  vp_v3(HALF_W, THICK / 2, len / 2), 255, 200, 60);
    p->belt = -speed;
    b->z += len;
}

/* Stepping stones over the void, at different heights. */
static void sec_steps(Builder *b, int rows)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2;
    for (int i = 0; i < rows; i++) {
        float z = b->z + 1.3f + 0.8f + i * 3.2f;
        for (int k = -1; k <= 1; k++) {
            float x = k * 3.6f + (rnd(&b->rng) - 0.5f) * 1.2f;
            float top = 0.2f + 0.7f * rnd(&b->rng);
            Part *p = add(b, P_BOX, vp_v3(x, top - 1.5f, z), vp_v3(0.8f, 1.5f, 0.8f), 160, 120, 255);
            (void)p;
        }
    }
    b->z += rows * 3.2f + 1.3f;
}

/* Seesaws over the void: planks tipping sideways under the beans, staggered
 * left and right, a jump apart. */
static void sec_seesaws(Builder *b, int n)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2;
    for (int i = 0; i < n; i++) {
        float x = (i & 1) ? 1.6f : -1.6f, len = 7.2f;
        Part *p = add(b, P_SEESAW, vp_v3(x, 0, b->z + 1.3f + len / 2), vp_v3(2.3f, 0.15f, len / 2), 0, 255, 140);
        p->floor = p->solid = 0;                    /* the physics has it */
        b->z += 1.3f + len;
    }
    b->z += 1.3f;
}

/* Big balls dropped at the far end, bouncing and rolling toward the start; a
 * trench at the near end catches them (the beans jump it). */
static void sec_bigballs(Builder *b, float len, float every)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2 + 2.8f;                               /* the trench: wider than a ball */
    slab(b, -HALF_W, HALF_W, b->z, b->z + len, 0);
    Part *p = add(b, P_BALLDROP, vp_v3(0, 0, b->z + len - 2.0f), vp_v3(HALF_W - 0.5f, 1.2f, 0), 255, 255, 255);
    p->floor = p->solid = 0;
    p->w = every;
    b->z += len;
}

/* A wall of light blocks across the course: barge through. */
static void sec_topple(Builder *b)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 10, 0);
    Part *p = add(b, P_TOPPLE, vp_v3(0, 0, b->z + 5), vp_v3(HALF_W, 2.0f, 0.5f), 255, 160, 40);
    p->floor = p->solid = 0;
    b->z += 10;
}

/* Walls of doors across the course, one row after another: some burst open
 * when a bean runs into them, the others are fake and stay shut. Which are
 * which changes every race. */
static void sec_doors(Builder *b, int rows)
{
    static const uint8_t colors[5][3] = {
        { 255, 80, 160 }, { 80, 200, 255 }, { 255, 200, 40 }, { 140, 255, 80 }, { 200, 120, 255 }
    };
    const float pitch = 2.4f, door = 0.96f, wall = 1.6f, deep = 0.2f, gap = 7.0f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 3 + rows * gap, 0);
    for (int r = 0; r < rows; r++) {
        float z = b->z + 3 + r * gap + gap / 2;
        int real = r == 0 ? 3 : 2, fake[5] = { 1, 1, 1, 1, 1 };
        for (int k = 0; k < real; ) {               /* `real` different doors open */
            int i = (int)(rnd(&b->rng) * 5) % 5;
            if (fake[i]) { fake[i] = 0; k++; }
        }
        for (int i = 0; i < 5; i++) {
            Part *d = add(b, P_DOOR, vp_v3((i - 2) * pitch, wall, z), vp_v3(door, wall, deep),
                          colors[i][0], colors[i][1], colors[i][2]);
            d->floor = 0;
            d->solid = 1;
            d->variant = fake[i];
        }
        for (int i = 0; i < 6; i++) {               /* the pillars between them */
            float x = (i - 2.5f) * pitch;
            Part *p = add(b, P_BOX, vp_v3(x, wall + 0.1f, z), vp_v3(pitch / 2 - door, wall + 0.1f, deep + 0.05f),
                          220, 220, 255);
            p->floor = 0;
        }
    }
    b->z += 3 + rows * gap;
}

/* A field of tiles over the void, five across: one safe way through (each
 * step straight on or sideways, never across a corner; and a few dead
 * ends); the rest are fake, shaking when stepped on and dropping away. A new
 * way every race. */
static void sec_tiles(Builder *b, int rows)
{
    const float pitch = 2.4f, half = 1.1f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2;
    /* the way: in each row, the tiles from where it came in to where it
     * goes on (a run sideways, then on to the next row) */
    int safe[16][5] = { { 0 } };
    if (rows > 16) rows = 16;
    int col = (int)(rnd(&b->rng) * 5) % 5;
    for (int r = 0; r < rows; r++) {
        int next = col;
        if (r + 1 < rows && rnd(&b->rng) < 0.6f) next = (int)(rnd(&b->rng) * 5) % 5;
        for (int i = col < next ? col : next; i <= (col > next ? col : next); i++) safe[r][i] = 1;
        if (rnd(&b->rng) < 0.3f) safe[r][(int)(rnd(&b->rng) * 5) % 5] = 1;     /* a dead end */
        col = next;
    }
    for (int r = 0; r < rows; r++) {
        float z = b->z + pitch / 2 + r * pitch;
        for (int i = 0; i < 5; i++) {
            Part *t = add(b, P_TILE, vp_v3((i - 2) * pitch, -0.25f, z), vp_v3(half, 0.25f, half),
                          170, 110, 255);
            t->variant = !safe[r][i];
        }
    }
    b->z += rows * pitch;
}

/* A pinball park: fat bumpers in staggered rows that bounce the beans away
 * (into each other, off the sides). */
static void sec_bumpers(Builder *b, int rows)
{
    const float gap = 3.4f;
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 3 + rows * gap, 0);
    for (int r = 0; r < rows; r++) {
        float z = b->z + 3 + r * gap + gap / 2;
        int odd = r & 1;
        for (int i = 0; i < (odd ? 2 : 3); i++) {
            float x = odd ? (i ? 1.8f : -1.8f) : (i - 1) * 3.6f;
            x += (rnd(&b->rng) - 0.5f) * 0.6f;
            float rad = 0.75f + 0.2f * rnd(&b->rng);
            add(b, P_BUMPER, vp_v3(x, 0, z), vp_v3(rad, 0.65f, rad), 255, 90, 60);
            b->c->parts[b->c->nparts - 1].floor = 0;
            b->c->parts[b->c->nparts - 1].solid = 0;    /* the physics has it */
        }
    }
    b->z += 3 + rows * gap;
}

/* A fruit chute: a belt running back toward the start while giant fruit
 * rolls down it, three lanes; a trench at the near end swallows the fruit
 * (the beans jump it). */
static void sec_fruit(Builder *b, float len, float belt, float speed)
{
    static const uint8_t fruit[3][3] = { { 255, 50, 50 }, { 255, 160, 0 }, { 120, 255, 60 } };
    static const float radius[3] = { 1.0f, 0.8f, 1.35f };
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W, HALF_W, b->z, b->z + 2, 0);
    b->z += 2 + 2.4f;                               /* the trench */
    Part *p = add(b, P_CONVEYOR, vp_v3(0, -THICK / 2, b->z + len / 2), vp_v3(HALF_W, THICK / 2, len / 2), 255, 200, 60);
    p->belt = -belt;
    static const float lanes[3] = { -3.6f, 0.0f, 3.6f };
    int first = (int)(rnd(&b->rng) * 3) % 3;
    for (int i = 0; i < 3; i++) {
        int k = (first + i) % 3;
        Part *r = add(b, P_ROLLER, vp_v3(lanes[i], 0, b->z + len), vp_v3(radius[k], 0, len + 1.2f),
                      fruit[k][0], fruit[k][1], fruit[k][2]);
        r->w = speed * (0.85f + 0.3f * rnd(&b->rng));
        r->ph = (len + 11.2f) * (i / 3.0f) + rnd(&b->rng) * 4.0f;
        r->variant = 1;
    }
    b->z += len;
}

static void sec_finish(Builder *b)
{
    checkpoint(b, -4, 4, 0);
    slab(b, -HALF_W - 1, HALF_W + 1, b->z, b->z + 14, 0)->g = 200;
    b->c->finish_z = b->z + 4.0f;
    add(b, P_ARCH, vp_v3(0, 0, b->c->finish_z), vp_v3(HALF_W + 0.5f, 5.0f, 0.4f), 255, 0, 255);
    b->z += 14;
}

/* The courses, and the round each is made for (0, 1, 2: the final). */
static const struct { const char *name; int round; } COURSES[] = {
    { "BEAN BOULEVARD", 0 },
    { "WOBBLE WAY",     1 },
    { "THE GAUNTLET",   2 },
    { "KNOCK KNOCK",    0 },
    { "PINBALL PARK",   0 },
    { "FRUIT SALAD",    1 },
    { "TIPPY TILES",    2 },
};
#define NCOURSES ((int)(sizeof COURSES / sizeof COURSES[0]))

int course_count(void) { return NCOURSES; }

int course_round(int id) { return COURSES[id < 0 || id >= NCOURSES ? 0 : id].round; }

const char *course_name(int id) { return COURSES[id < 0 || id >= NCOURSES ? 0 : id].name; }

void course_build(Course *c, int id, unsigned seed)
{
    Builder b;
    memset(c, 0, sizeof *c);
    b.c = c;
    b.z = 0;
    b.rng = seed * 2654435761u + (unsigned)id * 7919u;
    c->name = course_name(id);

    sec_start(&b);
    switch (id) {
    case 0:
    default:
        sec_flat(&b, 6);
        sec_spinners(&b, 2, 1.2f);
        sec_flat(&b, 3);
        sec_seesaws(&b, 2);
        sec_flat(&b, 3);
        sec_movers(&b, 2, 0.9f, 3.0f);
        sec_flat(&b, 3);
        sec_pendulums(&b, 2, 1.2f);
        sec_flat(&b, 3);
        sec_topple(&b);
        sec_pushers(&b, 2, 0.9f);
        sec_flat(&b, 3);
        break;
    case 1:
        sec_conveyor(&b, 10, 2.5f);
        sec_spinners(&b, 2, 1.7f);
        sec_flat(&b, 3);
        sec_movers(&b, 2, 1.1f, 3.5f);
        sec_flat(&b, 3);
        sec_seesaws(&b, 2);
        sec_pushers(&b, 2, 1.2f);
        sec_bigballs(&b, 20, 1.8f);
        break;
    case 2:
        sec_spinners(&b, 3, 2.0f);
        sec_movers(&b, 2, 1.4f, 4.0f);
        sec_pendulums(&b, 2, 1.5f);
        sec_seesaws(&b, 2);
        sec_conveyor(&b, 8, 3.0f);
        sec_bigballs(&b, 18, 1.4f);
        sec_steps(&b, 2);
        sec_topple(&b);
        sec_rollers(&b, 16, 8.0f);
        break;
    case 3:                                         /* KNOCK KNOCK */
        sec_flat(&b, 4);
        sec_doors(&b, 3);
        sec_flat(&b, 3);
        sec_spinners(&b, 2, 1.3f);
        sec_flat(&b, 3);
        sec_pushers(&b, 2, 1.0f);
        sec_doors(&b, 2);
        sec_flat(&b, 3);
        break;
    case 4:                                         /* PINBALL PARK */
        sec_flat(&b, 4);
        sec_bumpers(&b, 5);
        sec_flat(&b, 3);
        sec_movers(&b, 2, 0.9f, 3.0f);
        sec_flat(&b, 3);
        sec_pendulums(&b, 2, 1.2f);
        sec_bumpers(&b, 4);
        sec_topple(&b);
        sec_flat(&b, 3);
        break;
    case 5:                                         /* FRUIT SALAD */
        sec_flat(&b, 4);
        sec_fruit(&b, 16, 2.0f, 6.0f);
        sec_flat(&b, 3);
        sec_doors(&b, 2);
        sec_seesaws(&b, 2);
        sec_bumpers(&b, 3);
        sec_fruit(&b, 14, 2.5f, 7.5f);
        sec_flat(&b, 3);
        break;
    case 6:                                         /* TIPPY TILES */
        sec_spinners(&b, 2, 1.8f);
        sec_tiles(&b, 6);
        sec_flat(&b, 3);
        sec_bumpers(&b, 3);
        sec_movers(&b, 2, 1.3f, 4.0f);
        sec_doors(&b, 2);
        sec_tiles(&b, 5);
        sec_flat(&b, 3);
        break;
    }
    sec_finish(&b);
    c->length = b.z;
}

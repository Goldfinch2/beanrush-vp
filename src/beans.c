/* beans.c — the beans: running, jumping, diving, being knocked over, falling
 * off and coming back at the last checkpoint; and the computer's beans, which
 * look ahead in time (the course's parts move by the clock) to time their
 * jumps and waits. Each bean is a body in the physics world (props.c): it
 * steers its body's velocity, and the physics carries it, shoves it into the
 * others, and knocks it over.
 */
#include "beanrush.h"

#include <math.h>
#include <string.h>

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float bean_progress(const Bean *b)
{
    if (b->state == BEAN_FINISHED) return 10000.0f - b->place;
    return b->fallen > 0 ? b->best_z - 1.0f : b->pos.z;
}

/* At the start: a grid at the back of the start platform. */
void bean_place(Bean *b, const Course *c, int slot, int count)
{
    (void)c;
    int cols = count > 8 ? 4 : count > 3 ? 3 : count;
    int col = slot % cols, row = slot / cols;
    float spacing = 2.4f;
    b->pos = vp_v3((col - (cols - 1) / 2.0f) * spacing, 0, 1.5f + row * 1.6f);
    b->vel = vp_v3(0, 0, 0);
    b->heading = 0;
    b->grounded = 1;
    b->stun = b->dive = b->fallen = b->squash = 0;
    b->check = 0;
    b->state = BEAN_RACING;
    b->place = 0;
    b->finish_time = 0;
    b->hold = 0;
    b->think = 0;
    b->stuck = 0;
    b->best_z = b->pos.z;
    b->temper = 0;
    b->grudge = -1;
    b->revenge = b->punch = b->cool = 0;
}

static void respawn(Bean *b, const Course *c, float t)
{
    const Checkpoint *k = &c->checks[b->check];
    /* somewhere across the checkpoint: by its lane and the clock, so two
     * beans falling together do not land on each other */
    float x = k->x0 + (k->x1 - k->x0) * fmodf(b->lane * 0.5f + 0.5f + t * 0.37f, 1.0f);
    b->pos = vp_v3(x, k->y + 0.3f, k->z);
    b->vel = vp_v3(0, 0, 0);
    b->heading = 0;
    b->stun = 0.3f;
    b->dive = 0;
    b->fallen = 0;
    b->grounded = 0;
    b->tumbling = 0;
    props_bean_put(b);
}

/* Back on its feet after tumbling: upright where it is, turning locked again. */
static void stand_up(Bean *b)
{
    b->tumbling = 0;
    b->dive = 0;
    b->heading = atan2f(b->vel.x, b->vel.z + 1e-4f);
    props_bean_put(b);
}

/* Before the physics step: steer the bean's body from its controls. Its
 * body carries it (contacts, being shoved, what it stands on): only its
 * velocity is set here, as an arcade character runs. */
void bean_control(Bean *b, const BeanInput *in, const Course *c, float t, float dt, int *events)
{
    *events = 0;
    if (b->state == BEAN_OUT) return;
    if (b->fallen > 0) {
        b->fallen += dt;
        if (b->fallen >= RESPAWN_TIME && b->state == BEAN_RACING) respawn(b, c, t);
        return;
    }
    if (b->stun > 0) b->stun -= dt;
    if (b->tumbling) {                              /* knocked over: the physics has it */
        if (b->stun <= 0) stand_up(b);
        return;
    }

    /* ---- the controls ---- */
    float mx = in->x, mz = in->z;
    if (b->state != BEAN_RACING) mx = mz = 0;       /* through: it stops */
    float m = sqrtf(mx * mx + mz * mz);
    if (m > 1) { mx /= m; mz /= m; m = 1; }
    if (b->stun > 0) { mx = mz = 0; m = 0; }
    if (b->dive > 0) {
        b->dive -= dt;
        mx = mz = 0; m = 0;
    } else if (b->stun <= 0 && b->state == BEAN_RACING) {
        if (in->jump && b->grounded) {
            b->vel.y = JUMP_SPEED + b->ground.carry.y;
            b->grounded = 0;
            b->squash = 1.0f;
            *events |= EV_JUMP;
        } else if (in->dive) {
            float fx = sinf(b->heading), fz = cosf(b->heading);
            if (m > 0.2f) { fx = mx / m; fz = mz / m; }
            b->vel.x = fx * DIVE_SPEED;
            b->vel.z = fz * DIVE_SPEED;
            b->vel.y = b->grounded ? 4.5f : fmaxf(b->vel.y, 2.0f);
            b->heading = atan2f(fx, fz);
            b->grounded = 0;
            b->dive = DIVE_TIME;
            b->stun = DIVE_TIME;                     /* down until it gets up */
            props_bean_dive(b);
            *events |= EV_DIVE;
            return;
        }
    }

    /* ---- running: toward the wanted velocity, over what it stands on ---- */
    vp_vec3 carry = b->grounded ? b->ground.carry : vp_v3(0, 0, 0);
    float want_x = mx * RUN_SPEED, want_z = mz * RUN_SPEED;
    float acc = (b->grounded ? ACCEL_GROUND : ACCEL_AIR) * dt;
    if (b->dive > 0 || b->stun > 0) {
        acc = b->grounded ? 18.0f * dt : 0;         /* sliding to a stop */
        want_x = want_z = 0;
    }
    float rx = b->vel.x - carry.x, rz = b->vel.z - carry.z;
    float dx = want_x - rx, dz = want_z - rz;
    float dl = sqrtf(dx * dx + dz * dz);
    if (dl > acc) { dx *= acc / dl; dz *= acc / dl; }
    b->vel.x = rx + dx + carry.x;
    b->vel.z = rz + dz + carry.z;
    /* a tilted prop (a seesaw): sliding toward its low side */
    if (b->grounded && b->ground.body && b->ground.normal.y < 0.995f) {
        b->vel.x += b->ground.normal.x * GRAVITY * 1.3f * dt;
        b->vel.z += b->ground.normal.z * GRAVITY * 1.3f * dt;
    }
    if (m > 0.2f && b->dive <= 0 && b->stun <= 0) {
        float h = atan2f(mx, mz), d = h - b->heading;
        while (d > 3.14159f) d -= 6.28318f;
        while (d < -3.14159f) d += 6.28318f;
        b->heading += clampf(d, -12.0f * dt, 12.0f * dt);
    }
    props_bean_drive(b, dt);
}

/* After the physics step: where the body took it; what it stands on; the
 * checkpoints, the finish, falling off. */
void bean_sync(Bean *b, const Course *c, float t, float dt, int *events)
{
    (void)t;
    if (b->state == BEAN_OUT || b->fallen > 0) return;
    float vy_before = b->vel.y;
    props_bean_read(b);

    int was = b->grounded;
    Support s;
    b->grounded = 0;
    if (!b->tumbling && props_bean_ground(b, &s) && b->vel.y <= s.carry.y + 0.5f) {
        b->grounded = 1;
        b->ground = s;
        if (!was && vy_before < -6) { b->squash = -0.6f; *events |= EV_LAND; }
    } else {
        b->ground.part = -1;
        b->ground.body = NULL;
        b->ground.carry = vp_v3(0, 0, 0);
    }
    b->squash *= 0.85f;

    if (b->state != BEAN_RACING) return;
    if (b->grounded) {
        while (b->check + 1 < c->nchecks && b->pos.z > c->checks[b->check + 1].z)
            b->check++;
        if (b->pos.z > b->best_z) b->best_z = b->pos.z;
    }
    if (b->pos.z >= c->finish_z && b->pos.y > -1.0f)
        b->state = BEAN_FINISHED;
    if (b->pos.y < KILL_Y) {
        b->fallen = dt;
        b->tumbling = 0;
        props_bean_put(b);                          /* parked until it comes back */
        *events |= EV_FALL;
    }
}

/* ---- the computer's beans ---------------------------------------------------------- */

static int floor_at(const Course *c, float x, float z, float y, float t)
{
    Support s;
    return course_floor(c, x, z, y + 1.0f, t, &s) && s.top > y - 1.5f;
}

/* Would a hazard hit a bean running on from pos at velocity v, in the next
 * `ahead` seconds (feet raised by `lift`)? */
static int danger(const Course *c, vp_vec3 pos, vp_vec3 v, float lift, float t, float ahead)
{
    vp_vec3 k;
    for (float s = 0.1f; s <= ahead; s += 0.1f) {
        vp_vec3 p = vp_v3(pos.x + v.x * s, pos.y + lift, pos.z + v.z * s);
        if (course_hazard(c, p, t + s, &k)) return 1;
    }
    return 0;
}

/* A wall of doors ahead (within 8 m): the x of the door to make for (one
 * already open, else one not known to be fake: each bean its own guess),
 * and how far off the wall is. 0: none. */
static int door_ahead(const Bean *b, const Course *c, float *x, float *dist)
{
    float wall = 1e9f;
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (p->type != P_DOOR) continue;
        float dz = p->c.z - p->h.z - b->pos.z;
        if (dz > -0.3f && dz < 8.0f && p->c.z < wall) wall = p->c.z;
    }
    if (wall > 1e8f) return 0;
    float best = 1e9f;
    int found = 0;
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (p->type != P_DOOR || fabsf(p->c.z - wall) > 0.01f) continue;
        float cost;
        if (props_part_gone(i)) cost = fabsf(p->c.x - b->pos.x) * 0.5f;          /* open: go */
        else if (props_part_known(i)) continue;                                /* fake */
        else cost = 6.0f + fabsf(p->c.x - b->lane * 4.0f) + (float)((b->id * 7 + i * 13) % 5) * 0.9f;
        if (cost < best) { best = cost; *x = p->c.x; found = 1; }
        *dist = p->c.z - p->h.z - b->pos.z;
    }
    return found;
}

/* On or before a field of tiles: the x of the tile to step to next, on the
 * cheapest way over the field (tiles seen to be safe cheap, unknown ones a
 * guess, each bean guessing its own way; shaking and dropped ones not at
 * all), and whether that tile is a guess and is on the same row. */
static int tile_ahead(const Bean *b, const Course *c, float *x, int *guess, int *sideways)
{
    enum { COLS = 5, MAXR = 16 };
    const float pitch = 2.4f;
    int start = -1;
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (p->type != P_TILE) continue;
        float dz = p->c.z - b->pos.z;
        if (dz > -1.3f && dz < 3.6f) { start = i; break; }
    }
    if (start < 0) return 0;
    while (start > 0 && c->parts[start - 1].type == P_TILE) start--;
    int rows = 0;
    while (rows < MAXR && start + (rows + 1) * COLS <= c->nparts && c->parts[start + rows * COLS].type == P_TILE)
        rows++;
    const float z0 = c->parts[start].c.z;
    int rb = (int)floorf((b->pos.z - z0) / pitch + 0.5f);
    if (b->pos.z < z0 - 1.2f) rb = -1;
    if (rb >= rows) return 0;
    int cb = (int)floorf((b->pos.x + 2 * pitch) / pitch + 0.5f);
    cb = cb < 0 ? 0 : cb >= COLS ? COLS - 1 : cb;

    /* the cheapest way from the far end back (Dijkstra over a few tiles) */
    float cost[MAXR][COLS];
    int done[MAXR][COLS] = { { 0 } };
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < COLS; k++) cost[r][k] = 1e9f;
    for (int k = 0; k < COLS; k++) cost[rows - 1][k] = 0;
    for (;;) {
        int br = -1, bk = 0;
        for (int r = 0; r < rows; r++)
            for (int k = 0; k < COLS; k++)
                if (!done[r][k] && cost[r][k] < 1e8f && (br < 0 || cost[r][k] < cost[br][bk])) { br = r; bk = k; }
        if (br < 0) break;
        done[br][bk] = 1;
        int i = start + br * COLS + bk;
        if (props_part_gone(i) || (props_part_age(i) >= 0 && !(br == rb && bk == cb))) continue;   /* no way through */
        float step = props_part_known(i) ? 1.0f : 3.0f + (float)((b->id * 5 + i * 11) % 7) * 0.4f;
        static const int nr[4] = { -1, 1, 0, 0 }, nk[4] = { 0, 0, -1, 1 };
        for (int n = 0; n < 4; n++) {
            int r = br + nr[n], k = bk + nk[n];
            if (r < 0 || r >= rows || k < 0 || k >= COLS || done[r][k]) continue;
            if (cost[br][bk] + step < cost[r][k]) cost[r][k] = cost[br][bk] + step;
        }
    }
    /* the next tile: from the bean's own, the neighbor with the cheapest way
     * on (before the field: the first row, by the way and how far across) */
    float best = 1e9f;
    int nr = -1, nk = -1;
    if (rb < 0) {
        for (int k = 0; k < COLS; k++) {
            int i = start + k;
            if (props_part_gone(i) || props_part_age(i) >= 0) continue;
            float v = cost[0][k] + (props_part_known(i) ? 1.0f : 3.0f) + fabsf((float)(k - cb)) * 0.8f;
            if (v < best) { best = v; nr = 0; nk = k; }
        }
    } else {
        static const int dr[4] = { 1, 0, 0, -1 }, dk[4] = { 0, -1, 1, 0 };
        for (int n = 0; n < 4; n++) {
            int r = rb + dr[n], k = cb + dk[n];
            if (r < 0 || k < 0 || k >= COLS) continue;
            if (r >= rows) { best = 0; nr = r; nk = k; break; }       /* off the far end */
            int i = start + r * COLS + k;
            if (props_part_gone(i) || props_part_age(i) >= 0) continue;
            float v = cost[r][k] + (props_part_known(i) ? 1.0f : 3.0f);
            if (v < best) { best = v; nr = r; nk = k; }
        }
    }
    if (nr < 0) return 0;
    *x = (nk - 2) * pitch;
    *guess = nr < rows && !props_part_known(start + nr * COLS + nk);
    *sideways = nr == rb;
    return 1;
}

/* A bumper in the way of a bean heading for tx: a way round it. */
static float round_bumpers(const Bean *b, const Course *c, float tx)
{
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        if (p->type != P_BUMPER) continue;
        float dz = p->c.z - b->pos.z;
        if (dz < 0 || dz > 4.5f) continue;
        float clear = p->h.x + BEAN_R + 0.35f;
        float along = b->pos.x + (tx - b->pos.x) * fminf(dz / 4.5f, 1.0f);
        if (fabsf(along - p->c.x) >= clear) continue;
        float left = p->c.x - clear, right = p->c.x + clear;
        tx = fabsf(b->pos.x - left) < fabsf(b->pos.x - right) ? left : right;
        if (tx < -5.2f) tx = right;
        if (tx > 5.2f) tx = left;
    }
    return tx;
}

void bean_ai(Bean *b, const Course *c, float t, BeanInput *out, unsigned *rng)
{
    memset(out, 0, sizeof *out);
    if (b->state != BEAN_RACING || b->fallen > 0 || b->stun > 0 || b->dive > 0) return;

    /* now and then: a new lane, and whether it judges well this time */
    b->think -= 1.0f / 60.0f;
    if (b->think <= 0) {
        b->think = 0.8f + rnd(rng) * 1.2f;
        b->sloppy = rnd(rng) > b->skill;
        if (rnd(rng) < 0.35f) b->lane = clampf(b->lane + (rnd(rng) - 0.5f) * 0.8f, -0.8f, 0.8f);
    }

    float speed = RUN_SPEED * b->skill;
    float tx = b->lane * 4.0f;
    if (b->grounded && b->ground.body)
        tx = vp_body_pos(b->ground.body).x;          /* on a seesaw: its middle, to keep it level */
    out->z = b->skill;
    float dx = 0, dist = 99;
    int guess = 0, sideways = 0;
    if (door_ahead(b, c, &dx, &dist)) {
        tx = dx;
        if (dist < 2.0f && fabsf(dx - b->pos.x) > 0.5f) out->z = 0.25f;     /* line up first */
    } else if (tile_ahead(b, c, &dx, &guess, &sideways)) {
        tx = dx;
        if (guess) out->z = fminf(out->z, 0.6f);     /* careful on the unknown */
        if (sideways) out->z = 0;                    /* across first, then on */
        else if (fabsf(dx - b->pos.x) > 0.8f) out->z = fminf(out->z, 0.25f);
    }
    tx = round_bumpers(b, c, tx);
    out->x = clampf((tx - b->pos.x) * 0.35f, -0.6f, 0.6f);
    if (fabsf(tx - b->pos.x) > 1.0f) out->x = clampf((tx - b->pos.x) * 0.5f, -0.9f, 0.9f);

    if (!b->grounded) return;
    vp_vec3 v = vp_v3(out->x * RUN_SPEED, 0, speed);

    /* a gap ahead: jump if there is something to land on, else wait at the
     * edge for it to come (moving platforms) */
    float edge = 0.9f;
    if (!floor_at(c, b->pos.x, b->pos.z + edge, b->pos.y, t + 0.1f)) {
        const float air = 0.62f;
        int ok = 0;
        for (float land = 3.4f; land <= 4.4f && !ok; land += 1.0f)      /* near, else farther */
            for (float dx = 0; dx <= 1.6f && !ok; dx += 0.8f) {
                if (floor_at(c, b->pos.x + dx, b->pos.z + land, b->pos.y, t + air)) { ok = 1; out->x = dx * 0.3f; }
                else if (dx > 0 && floor_at(c, b->pos.x - dx, b->pos.z + land, b->pos.y, t + air)) { ok = 1; out->x = -dx * 0.3f; }
            }
        if (ok && !b->sloppy) {
            out->jump = 1;
            b->hold = 0;
        } else if (ok) {
            out->jump = rnd(rng) < 0.5f;             /* a bad take-off */
        } else {
            out->z = -0.15f;                         /* wait at the edge */
            b->hold += 1.0f / 60.0f;
            if (b->hold > 4.0f) { out->jump = 1; b->hold = 0; }   /* impatient */
        }
        return;
    }

    /* a step up ahead (stepping stones): jump onto it */
    {
        Support s;
        if (course_floor(c, b->pos.x, b->pos.z + 0.9f, b->pos.y + 2.0f, t, &s) &&
            s.top > b->pos.y + STEP_UP * 0.8f && s.top < b->pos.y + 1.4f) {
            out->jump = 1;
            return;
        }
    }

    /* a hazard coming: jump it if jumping clears it, else hold back */
    if (danger(c, b->pos, v, 0, t, 0.45f)) {
        if (!danger(c, b->pos, v, 1.0f, t, 0.45f)) {
            if (!b->sloppy) out->jump = 1;
        } else if (!b->sloppy) {
            if (!danger(c, b->pos, vp_v3(0, 0, 0), 0, t, 0.6f)) {
                out->z = 0;                          /* let it pass */
                b->hold += 1.0f / 60.0f;
                if (b->hold > 2.5f) { out->z = b->skill; b->hold = 0; }
            } else {
                out->x = b->pos.x > 0 ? -0.8f : 0.8f;  /* sidestep */
            }
        }
    }

    /* stuck against something: hop and try another lane */
    if (b->pos.z > b->best_z - 0.05f) b->stuck = 0;
    else b->stuck += 1.0f / 60.0f;
    if (b->stuck > 2.0f) {
        out->jump = 1;
        b->lane = -b->lane + (rnd(rng) - 0.5f) * 0.5f;
        b->stuck = 0;
    }
}

/* ---- moods ------------------------------------------------------------------------------ */

void bean_annoy(Bean *victim, int by, float amount)
{
    if (victim->state != BEAN_RACING) return;
    victim->temper += amount;
    if (victim->temper > 1.2f) victim->temper = 1.2f;
    if (amount > 0.05f) victim->grudge = by;
}

void bean_mood(Bean *b, Bean *beans, int n, float dt)
{
    if (b->punch > 0) b->punch -= dt;
    if (b->cool > 0) b->cool -= dt;
    if (b->revenge > 0) {
        b->revenge -= dt;
        const Bean *t = b->grudge >= 0 && b->grudge < n ? &beans[b->grudge] : NULL;
        if (!t || t->state != BEAN_RACING || t->fallen > 0 || b->fallen > 0) b->revenge = 0;
        return;
    }
    b->temper -= 0.07f * dt;                         /* cooling down */
    if (b->temper < 0) b->temper = 0;
    /* the computer's beans, furious: after whoever pushed them most */
    if (!b->human && b->temper >= 1.0f && b->grudge >= 0 && b->grudge < n &&
        beans[b->grudge].state == BEAN_RACING && b->state == BEAN_RACING) {
        b->revenge = 4.0f;
    }
}

int bean_strike(Bean *b, Bean *beans, int n, const Course *c, float t)
{
    if (b->cool > 0 || b->state != BEAN_RACING || b->fallen > 0 || b->stun > 0 || b->tumbling) return 0;
    float fx = sinf(b->heading), fz = cosf(b->heading);
    Bean *best = NULL;
    float bd = 1.9f;
    for (int i = 0; i < n; i++) {
        Bean *o = &beans[i];
        if (o == b || o->state != BEAN_RACING || o->fallen > 0) continue;
        float dx = o->pos.x - b->pos.x, dz = o->pos.z - b->pos.z, d = sqrtf(dx * dx + dz * dz);
        if (d > bd || fabsf(o->pos.y - b->pos.y) > 1.2f) continue;
        int grudge = b->revenge > 0 && i == b->grudge;
        if (!grudge && (dx * fx + dz * fz) < 0.3f * d) continue;       /* not in front */
        bd = d; best = o;
    }
    b->punch = 0.35f;
    b->cool = 1.2f;
    if (!best) return EV_WHIFF;
    float dx = best->pos.x - b->pos.x, dz = best->pos.z - b->pos.z, d = sqrtf(dx * dx + dz * dz) + 1e-4f;
    dx /= d; dz /= d;
    float fury = b->temper >= 1.0f || b->revenge > 0 ? 1.25f : 1.0f;
    /* at an edge: no floor a little past it, that way. A shove, over. */
    Support s;
    int edge = !course_floor(c, best->pos.x + dx * 1.8f, best->pos.z + dz * 1.8f, best->pos.y + 0.5f, t, &s) ||
               s.top < best->pos.y - 1.0f;
    best->temper += 0.3f;                            /* and now it holds a grudge */
    if (best->temper > 1.2f) best->temper = 1.2f;
    best->grudge = b->id;
    b->temper = 0;
    b->revenge = 0;
    if (edge) {
        props_bean_push(best, vp_v3(dx * 13.0f * fury, 4.5f, dz * 13.0f * fury), 0);
        return EV_SHOVE;
    }
    props_bean_push(best, vp_v3(dx * 6.5f * fury, 4.5f, dz * 6.5f * fury), 1);
    return EV_PUNCH;
}

/* Going after its grudge: straight at it, punching when in reach. */
void bean_revenge(Bean *b, const Bean *beans, BeanInput *out)
{
    memset(out, 0, sizeof *out);
    const Bean *t = &beans[b->grudge];
    float dx = t->pos.x - b->pos.x, dz = t->pos.z - b->pos.z, d = sqrtf(dx * dx + dz * dz) + 1e-4f;
    out->x = dx / d;
    out->z = dz / d;
    if (d < 1.6f) out->punch = 1;
    if (t->pos.y > b->pos.y + 0.8f && b->grounded) out->jump = 1;
}

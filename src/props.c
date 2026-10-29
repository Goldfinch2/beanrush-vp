/* props.c — the race's physics world (vp_physics): the course (its
 * platforms static; its moving platforms, pushers, spinning bars, wrecking
 * balls and rolling balls moved by the clock, so they shove what they meet),
 * the physical props (seesaws that tip, big balls that bounce and roll down
 * the course, walls of blocks to barge through), doors that burst open or
 * stay shut, tiles that drop away, bumpers that bounce, and a body for each
 * bean.
 *
 * A bean's body is a capsule held upright: the bean steers its velocity
 * (bean_control) and the physics does the rest, so the beans shove each
 * other with their weight, ride what moves, weigh down the seesaws, stand on
 * each other's heads. A hazard that hits a bean knocks it over: it tumbles
 * freely (turning unlocked) until it gets up.
 */
#include "beanrush.h"

#include <math.h>
#include <string.h>

#define BEAN_LIFT     0.75f     /* its capsule's middle above its feet */
#define MAX_SEESAWS   8
#define MAX_BALLS     12
#define MAX_DROPPERS  4
#define MAX_BLOCKS    48
#define MAX_MOVING    64
#define GONE_Y        -30.0f    /* fallen this far: taken away */
#define PARK_Y        -500.0f   /* where the beans out of the race wait */
#define TILE_SHAKE    0.6f      /* s a fake tile shakes before it drops */

/* What a body is (its user pointer): for the rays, the contacts. */
enum { K_COURSE, K_CONVEYOR, K_PROP, K_BALL, K_HAZARD, K_PUSHER, K_BEAN, K_DOOR, K_BUMPER };
typedef struct { int kind; int index; } Tag;    /* index: the part, or the bean */

typedef struct { vp_body *plank, *post; vp_joint *hinge; vp_vec3 half, post_half; } Seesaw;
typedef struct { vp_body *body; float radius; } Ball;
typedef struct { int part; float timer; } Dropper;
typedef struct { vp_body *body; vp_vec3 half; } Block;
typedef struct { vp_body *body; int part; } Moving;

/* A bean's arms and legs: fat capsules on hinges at its hips and shoulders,
 * swinging forward and back. The legs' motors run them in a stride; the
 * arms swing free. Knocked over or diving, all go limp. */
#define LIMBS     4                 /* 0, 1 the legs (left, right); 2, 3 the arms */
#define LIMB_MASS 2.0f
typedef struct { vp_body *body; vp_joint *hinge; } Limb;
typedef struct {
    vp_vec3 joint;                  /* hip or shoulder, in the bean's own frame (its middle at 0) */
    vp_vec3 dir;                    /* the limb hanging from it, a unit vector */
    float   radius, length;         /* fat; joint to tip */
} LimbShape;

static LimbShape limb_shape(int k)
{
    float side = (k & 1) ? 1.0f : -1.0f;
    LimbShape l;
    if (k < 2) {
        l.joint = vp_v3(side * 0.19f, -0.31f, 0);
        l.dir = vp_v3(0, -1, 0);
        l.radius = 0.11f;
        l.length = 0.38f;               /* the feet clear the floor by 6 cm: the body stands */
    } else {
        l.joint = vp_v3(side * 0.40f, 0.22f, 0);
        l.dir = vp_v3(side * 0.5f, -0.866f, 0);
        l.radius = 0.09f;
        l.length = 0.38f;
    }
    return l;
}

static struct {
    vp_phys *world;
    Tag      part_tags[MAX_PARTS];
    Tag      bean_tags[MAX_BEANS];
    Tag      prop_tag, ball_tag;
    const Course *course;
    Seesaw   seesaws[MAX_SEESAWS];
    int      nseesaws;
    Ball     balls[MAX_BALLS];
    Dropper  droppers[MAX_DROPPERS];
    int      ndroppers;
    Block    blocks[MAX_BLOCKS];
    int      nblocks;
    Moving   moving[MAX_MOVING];
    int      nmoving;
    Limb     limbs[MAX_BEANS][LIMBS];
    float    stride[MAX_BEANS];                 /* the legs' running phase */
    unsigned rng;
    float    t;                                 /* the race clock, at the last step */
    /* the doors, tiles and bumpers: their bodies, what has happened to them */
    vp_body *body[MAX_PARTS];
    uint8_t  gone[MAX_PARTS], known[MAX_PARTS], happened[MAX_PARTS];
    float    when[MAX_PARTS];
} S;

int props_part_gone(int part)  { return part >= 0 && part < MAX_PARTS && S.gone[part]; }
int props_part_known(int part) { return part >= 0 && part < MAX_PARTS && S.known[part]; }

float props_part_age(int part)
{
    if (part < 0 || part >= MAX_PARTS || !S.happened[part]) return -1.0f;
    return S.t - S.when[part];
}

static void happen(int part)
{
    S.happened[part] = 1;
    S.when[part] = S.t;
}

static void take_away(int part)
{
    if (S.body[part]) vp_body_remove(S.body[part]);
    S.body[part] = NULL;
    S.gone[part] = 1;
}

static const Tag *tag_of(const vp_body *b) { return (const Tag *)vp_body_user(b); }

/* ---- what is under a bean ------------------------------------------------------------- */

static void support_from(const vp_hit *h, Support *s)
{
    const Tag *t = tag_of(h->body);
    vp_vec3 c = vp_body_pos(h->body), v = vp_body_vel(h->body), w = vp_body_spin(h->body);
    s->part = (t && (t->kind == K_COURSE || t->kind == K_CONVEYOR || t->kind == K_PUSHER)) ? t->index : -1;
    s->body = (t && t->kind == K_COURSE) ? NULL : h->body;   /* NULL: the steady course */
    s->top = h->point.y;
    s->normal = h->normal;
    s->carry = vp_v3_add(v, vp_v3_cross(w, vp_v3_sub(h->point, c)));   /* the surface's own speed there */
    if (t && t->kind == K_CONVEYOR) s->carry.z += S.course->parts[t->index].belt;
}

int props_bean_ground(const Bean *b, Support *s)
{
    vp_hit h;
    vp_vec3 mid = vp_v3(b->pos.x, b->pos.y + BEAN_LIFT, b->pos.z);
    if (!S.world || !vp_phys_raycast_ignore(S.world, mid, vp_v3(mid.x, b->pos.y - 0.25f, mid.z), b->body, &h))
        return 0;
    if (h.normal.y < 0.5f) return 0;                /* a wall, not a floor */
    support_from(&h, s);
    return 1;
}

/* For the computer's beans (course_floor): a prop under (x, z), where it is now. */
static int props_floor(float x, float z, float y, Support *s)
{
    vp_hit h;
    if (!S.world || !vp_phys_raycast(S.world, vp_v3(x, y + 0.12f, z), vp_v3(x, y - 1.5f, z), &h))
        return 0;
    const Tag *t = tag_of(h.body);
    if (!t || t->kind != K_PROP || h.normal.y < 0.5f) return 0;
    support_from(&h, s);
    return 1;
}

/* ---- the beans' bodies ------------------------------------------------------------------- */

/* A turn about y by h: the bean's frame facing heading h (its +z ahead). */
static vp_vec3 turn_y(vp_vec3 v, float h)
{
    float c = cosf(h), sn = sinf(h);
    return vp_v3(c * v.x + sn * v.z, v.y, -sn * v.x + c * v.z);
}

/* A placement from three axes (columns) and a position. */
static vp_xf axes(vp_vec3 x, vp_vec3 y, vp_vec3 z, vp_vec3 at)
{
    vp_xf p;
    p.m[0] = x.x; p.m[1] = y.x; p.m[2] = z.x;
    p.m[3] = x.y; p.m[4] = y.y; p.m[5] = z.y;
    p.m[6] = x.z; p.m[7] = y.z; p.m[8] = z.z;
    p.t[0] = at.x; p.t[1] = at.y; p.t[2] = at.z;
    return p;
}

/* Limb k at rest, for a bean whose middle is at `mid` facing h: its capsule's
 * placement (its axis along y, pointing up the limb to the joint). */
static vp_xf limb_rest(int k, vp_vec3 mid, float h)
{
    LimbShape l = limb_shape(k);
    vp_vec3 up = vp_v3_scale(l.dir, -1.0f);
    vp_vec3 x = vp_v3(up.y, -up.x, 0), z = vp_v3(0, 0, 1);
    vp_vec3 centre = vp_v3_add(l.joint, vp_v3_scale(l.dir, l.length / 2));
    return axes(turn_y(x, h), turn_y(up, h), turn_y(z, h), vp_v3_add(mid, turn_y(centre, h)));
}

/* Put the bean upright where it is, facing its heading, its limbs hanging
 * at rest (or the whole bean parked out of the way, fallen or out). */
void props_bean_put(Bean *b)
{
    if (!b->body) return;
    int away = b->state == BEAN_OUT || b->fallen > 0;
    vp_vec3 mid = vp_v3(b->pos.x, away ? PARK_Y : b->pos.y + BEAN_LIFT, b->pos.z);
    vp_xf x = axes(turn_y(vp_v3(1, 0, 0), b->heading), vp_v3(0, 1, 0), turn_y(vp_v3(0, 0, 1), b->heading), mid);
    vp_body_set_place(b->body, &x);
    vp_body_set_vel(b->body, away ? vp_v3(0, 0, 0) : b->vel);
    vp_body_set_spin(b->body, vp_v3(0, 0, 0));
    vp_body_lock_rotation(b->body, 0, 1, 0);        /* upright; turning to face its way */
    vp_body_set_material(b->body, 0, 0);            /* it steers itself: no grip */
    vp_body_set_gravity(b->body, !away);
    for (int k = 0; k < LIMBS; k++) {
        vp_body *limb = S.limbs[b->id][k].body;
        if (!limb) continue;
        vp_xf p = limb_rest(k, mid, b->heading);
        vp_body_set_place(limb, &p);
        vp_body_set_vel(limb, away ? vp_v3(0, 0, 0) : b->vel);
        vp_body_set_spin(limb, vp_v3(0, 0, 0));
        vp_body_set_gravity(limb, !away);
    }
}

/* Knocked over, or diving: the whole bean goes limp and tumbles. */
static void go_limp(Bean *b, vp_vec3 spin)
{
    b->tumbling = 1;
    b->grounded = 0;
    vp_body_lock_rotation(b->body, 1, 1, 1);
    vp_body_set_material(b->body, 0.05f, 0.6f);     /* sliding to a stop on its belly */
    vp_body_set_spin(b->body, spin);
}

void props_bean_dive(Bean *b)
{
    /* pitching forward: about its right side */
    vp_vec3 right = turn_y(vp_v3(1, 0, 0), b->heading);
    vp_body_set_vel(b->body, b->vel);
    go_limp(b, vp_v3_scale(right, 7.0f));
}

void props_bean_drive(Bean *b, float dt)
{
    if (!b->body) return;
    vp_body_set_vel(b->body, b->vel);
    /* turning to face its heading */
    vp_xf p = vp_body_place(b->body);
    float yaw = atan2f(p.m[2], p.m[8]), d = b->heading - yaw;
    while (d > 3.14159f) d -= 6.28318f;
    while (d < -3.14159f) d += 6.28318f;
    float w = d / dt * 0.5f;
    if (w > 15) w = 15;
    if (w < -15) w = -15;
    vp_body_set_spin(b->body, vp_v3(0, w, 0));
    /* the beans fall at their own (arcade) gravity, the props at the world's */
    vp_body_force(b->body, vp_v3(0, -BEAN_MASS * (GRAVITY - PROP_GRAVITY), 0));
}

void props_bean_read(Bean *b)
{
    if (!b->body) return;
    vp_vec3 c = vp_body_pos(b->body);
    b->pos = vp_v3(c.x, c.y - BEAN_LIFT, c.z);
    b->vel = vp_body_vel(b->body);
}

int props_bean_shadow(const Bean *b, float *floor_y)
{
    vp_hit h;
    if (!S.world || !b->body) return 0;
    vp_vec3 mid = vp_v3(b->pos.x, b->pos.y + BEAN_LIFT, b->pos.z);
    if (!vp_phys_raycast_ignore(S.world, mid, vp_v3(mid.x, mid.y - 12.0f, mid.z), b->body, &h)) return 0;
    if (h.normal.y < 0.5f) return 0;
    *floor_y = h.point.y;
    return 1;
}

int props_limb(const Bean *b, int k, vp_vec3 *joint, vp_vec3 *tip, float *radius)
{
    if (!b->body || k < 0 || k >= LIMBS || !S.limbs[b->id][k].body) return 0;
    LimbShape l = limb_shape(k);
    vp_xf p = vp_body_place(S.limbs[b->id][k].body);
    vp_vec3 up = vp_v3(p.m[1], p.m[4], p.m[7]), c = vp_v3(p.t[0], p.t[1], p.t[2]);
    *joint = vp_v3_add(c, vp_v3_scale(up, l.length / 2));
    *tip = vp_v3_sub(c, vp_v3_scale(up, l.length / 2));
    *radius = l.radius;
    return 1;
}

/* ---- building ------------------------------------------------------------------------------- */

static vp_body *add_box(vp_body_kind kind, vp_vec3 c, vp_vec3 h, float mass, Tag *tag)
{
    vp_body *b = vp_phys_add_box(S.world, kind, c, h, mass);
    if (b) vp_body_set_user(b, tag);
    return b;
}

static Moving *add_moving(int part)
{
    if (S.nmoving >= MAX_MOVING) return NULL;
    Moving *m = &S.moving[S.nmoving++];
    m->part = part;
    return m;
}

/* A bar's placement at angle a (its long side along (cos a, 0, sin a)). */
static vp_xf bar_place(vp_vec3 c, float a)
{
    vp_xf x = vp_xf_identity();
    float ca = cosf(a), sa = sinf(a);
    x.m[0] = ca; x.m[2] = -sa;
    x.m[6] = sa; x.m[8] = ca;
    x.t[0] = c.x; x.t[1] = c.y; x.t[2] = c.z;
    return x;
}

static vp_vec3 pendulum_ball(const Part *p, float t)
{
    float a = pendulum_angle(p, t);
    return vp_v3(p->c.x + sinf(a) * p->h.y, p->c.y - cosf(a) * p->h.y, p->c.z);
}

void props_free(void)
{
    if (S.world) vp_phys_free(S.world);
    memset(&S, 0, sizeof S);
    g_extra_floor = NULL;
}

void props_build(const Course *c, Bean *beans, int n, unsigned seed)
{
    props_free();
    if (vp_phys_create(&S.world) != VP_OK) return;
    vp_phys_set_gravity(S.world, vp_v3(0, -PROP_GRAVITY, 0));
    S.course = c;
    S.rng = seed;
    S.prop_tag.kind = K_PROP;
    S.ball_tag.kind = K_BALL;
    for (int i = 0; i < c->nparts; i++) {
        const Part *p = &c->parts[i];
        Tag *tag = &S.part_tags[i];
        tag->index = i;
        Moving *m;
        switch (p->type) {
        case P_BOX:
            tag->kind = K_COURSE;
            if (p->floor || p->solid) add_box(VP_BODY_STATIC, p->c, p->h, 0, tag);
            break;
        case P_CONVEYOR:
            tag->kind = K_CONVEYOR;
            add_box(VP_BODY_STATIC, p->c, p->h, 0, tag);
            break;
        case P_MOVER:                               /* a platform, or a pusher (no floor) */
            tag->kind = p->floor ? K_COURSE : K_PUSHER;
            if ((m = add_moving(i)))
                m->body = add_box(VP_BODY_MOVED, part_pos(p, 0), p->h, 0, tag);
            break;
        case P_SPINNER:
            tag->kind = K_HAZARD;
            if ((m = add_moving(i))) {
                vp_vec3 at = vp_v3(p->c.x, p->c.y + p->h.y, p->c.z);
                m->body = add_box(VP_BODY_MOVED, at, vp_v3(p->h.x, 0.2f, 0.2f), 0, tag);
                vp_xf x = bar_place(at, spinner_angle(p, 0));
                vp_body_set_place(m->body, &x);
            }
            break;
        case P_PENDULUM:
            tag->kind = K_HAZARD;
            if ((m = add_moving(i))) {
                m->body = vp_phys_add_sphere(S.world, VP_BODY_MOVED, pendulum_ball(p, 0), p->h.x, 0);
                vp_body_set_user(m->body, tag);
            }
            break;
        case P_ROLLER:
            tag->kind = K_HAZARD;
            if ((m = add_moving(i))) {
                m->body = vp_phys_add_sphere(S.world, VP_BODY_MOVED, vp_v3(p->c.x, PARK_Y, p->c.z), p->h.x, 0);
                vp_body_set_user(m->body, tag);
            }
            break;
        case P_SEESAW:
            if (S.nseesaws < MAX_SEESAWS) {
                Seesaw *s = &S.seesaws[S.nseesaws++];
                s->half = p->h;
                vp_vec3 mid = vp_v3(p->c.x, p->c.y - p->h.y, p->c.z);
                s->plank = add_box(VP_BODY_DYNAMIC, mid, p->h, 160, &S.prop_tag);
                vp_body_set_damping(s->plank, 0, 1.5f);
                vp_body_set_material(s->plank, 0, 0.8f);
                vp_body_set_sleep(s->plank, 0);
                float top = mid.y - p->h.y - 0.05f, bottom = -9.0f;
                s->post_half = vp_v3(0.3f, (top - bottom) / 2, 0.7f);
                tag->kind = K_COURSE;
                s->post = add_box(VP_BODY_STATIC, vp_v3(p->c.x, (top + bottom) / 2, p->c.z), s->post_half, 0, tag);
                s->hinge = vp_phys_hinge(S.world, s->plank, NULL, mid, vp_v3(0, 0, 1));
                vp_joint_limits(s->hinge, 1, -0.55f, 0.55f);
            }
            break;
        case P_BALLDROP:
            if (S.ndroppers < MAX_DROPPERS) {
                S.droppers[S.ndroppers].part = i;
                S.droppers[S.ndroppers].timer = 1.0f + rnd(&S.rng) * p->w;
                S.ndroppers++;
            }
            break;
        case P_TOPPLE: {
            float size = p->h.z * 2;
            int cols = (int)(p->h.x * 2 / size), rows = (int)(p->h.y / size);
            for (int r = 0; r < rows; r++)
                for (int k = 0; k < cols && S.nblocks < MAX_BLOCKS; k++) {
                    Block *b = &S.blocks[S.nblocks++];
                    b->half = vp_v3(p->h.z * 0.96f, p->h.z * 0.96f, p->h.z * 0.96f);
                    vp_vec3 at = vp_v3(p->c.x - p->h.x + size * (k + 0.5f) + ((r & 1) ? size / 2 : 0),
                                       p->c.y + size * (r + 0.5f), p->c.z);
                    if (at.x > p->c.x + p->h.x) at.x -= size * cols;    /* the odd rows, staggered */
                    b->body = add_box(VP_BODY_DYNAMIC, at, b->half, 6, &S.prop_tag);
                    vp_body_set_material(b->body, 0.05f, 0.7f);
                }
            break;
        }
        case P_DOOR:
            tag->kind = K_DOOR;
            S.body[i] = add_box(VP_BODY_STATIC, p->c, p->h, 0, tag);
            break;
        case P_TILE:
            tag->kind = K_COURSE;
            S.body[i] = add_box(VP_BODY_STATIC, p->c, p->h, 0, tag);
            break;
        case P_BUMPER:
            tag->kind = K_BUMPER;
            S.body[i] = vp_phys_add_capsule(S.world, VP_BODY_STATIC, vp_v3(p->c.x, p->c.y + p->h.y, p->c.z),
                                            p->h.x, fmaxf(2 * p->h.y - 2 * p->h.x, 0.05f), 0);
            if (S.body[i]) vp_body_set_user(S.body[i], tag);
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < n && i < MAX_BEANS; i++) {
        Bean *b = &beans[i];
        b->id = i;
        S.bean_tags[i].kind = K_BEAN;
        S.bean_tags[i].index = i;
        vp_vec3 mid = vp_v3(b->pos.x, b->pos.y + BEAN_LIFT, b->pos.z);
        b->body = vp_phys_add_capsule(S.world, VP_BODY_DYNAMIC, mid, BEAN_R, BEAN_H - 2 * BEAN_R, BEAN_MASS);
        vp_body_set_user(b->body, &S.bean_tags[i]);
        vp_body_set_sleep(b->body, 0);
        vp_body_set_damping(b->body, 0, 2.0f);
        b->tumbling = 0;
        /* the arms and legs, hinged at the hips and shoulders (swinging
         * forward and back; the parts of one bean do not collide) */
        for (int k = 0; k < LIMBS; k++) {
            LimbShape l = limb_shape(k);
            vp_xf rest = limb_rest(k, mid, b->heading);
            vp_vec3 at = vp_v3(rest.t[0], rest.t[1], rest.t[2]);
            vp_body *limb = vp_phys_add_capsule(S.world, VP_BODY_DYNAMIC, at, l.radius,
                                                l.length - 2 * l.radius, LIMB_MASS);
            if (!limb) continue;
            vp_body_set_place(limb, &rest);
            vp_body_set_user(limb, &S.bean_tags[i]);
            vp_body_set_sleep(limb, 0);
            vp_body_set_damping(limb, 0, k < 2 ? 0.5f : 1.2f);
            vp_body_set_material(limb, 0.1f, 0.6f);
            vp_vec3 joint = vp_v3_add(mid, turn_y(l.joint, b->heading));
            vp_joint *h = vp_phys_hinge(S.world, limb, b->body, joint, turn_y(vp_v3(1, 0, 0), b->heading));
            if (k < 2) vp_joint_limits(h, 1, -1.3f, 1.1f);      /* a leg: well forward, well back */
            else       vp_joint_limits(h, 1, -2.2f, 2.2f);      /* an arm: up to over its head */
            S.limbs[i][k].body = limb;
            S.limbs[i][k].hinge = h;
        }
        props_bean_put(b);
    }
    g_extra_floor = props_floor;
}

/* ---- each step ------------------------------------------------------------------------------ */

static void move_to(vp_body *b, vp_vec3 target, float dt)
{
    vp_vec3 d = vp_v3_sub(target, vp_body_pos(b));
    if (vp_v3_length(d) > 4.0f) {                   /* a jump: put it there */
        vp_body_set_pos(b, target);
        vp_body_set_vel(b, vp_v3(0, 0, 0));
    } else {
        vp_body_set_vel(b, vp_v3_scale(d, 1.0f / dt));
    }
}

static void drop_ball(const Part *p)
{
    for (int i = 0; i < MAX_BALLS; i++) {
        Ball *b = &S.balls[i];
        if (b->body) continue;
        float r = p->h.y;
        vp_vec3 at = vp_v3(p->c.x + (rnd(&S.rng) * 2 - 1) * (p->h.x - r), p->c.y + r + 3.0f, p->c.z);
        b->body = vp_phys_add_sphere(S.world, VP_BODY_DYNAMIC, at, r, 90);
        if (!b->body) return;
        vp_body_set_user(b->body, &S.ball_tag);
        vp_body_set_material(b->body, 0.35f, 0.4f);
        vp_body_set_vel(b->body, vp_v3((rnd(&S.rng) - 0.5f) * 2.0f, 0, -7.0f));
        b->radius = r;
        return;
    }
}

/* Knocked over: it tumbles, turning freely, popped up a little. */
static void knock(Bean *b, float away_x, float away_z)
{
    b->stun = 1.2f;
    b->dive = 0;
    vp_vec3 v = vp_body_vel(b->body);
    if (v.y < 5.0f) v.y = 5.0f;
    vp_body_set_vel(b->body, v);
    go_limp(b, vp_v3(away_z * 4.0f, 2.0f, -away_x * 4.0f));
}

/* Two beans bumping: sprung apart (harder the faster they met) with a hop,
 * squashing. Their own steering brings them back in a moment: a boing, not
 * a tumble. */
static void bounce(Bean *a, Bean *b, float speed)
{
    float dx = b->pos.x - a->pos.x, dz = b->pos.z - a->pos.z, l = sqrtf(dx * dx + dz * dz);
    if (l < 1e-3f) { dx = 1; dz = 0; l = 1; }
    dx /= l; dz /= l;
    float k = fminf(3.5f + speed * 0.9f, 9.0f);
    /* who pushed whom: the one moving into the other more */
    float ia = a->vel.x * dx + a->vel.z * dz, ib = -(b->vel.x * dx + b->vel.z * dz);
    float hurt = 0.10f + speed * 0.025f;
    if (ia >= ib) { bean_annoy(b, a->id, hurt); bean_annoy(a, b->id, hurt * 0.2f); }
    else          { bean_annoy(a, b->id, hurt); bean_annoy(b, a->id, hurt * 0.2f); }
    Bean *both[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        Bean *x = both[i];
        if (x->state == BEAN_OUT || x->fallen > 0 || x->tumbling) continue;
        if (x->punch > 0) continue;                 /* its fist met it: the punch did the pushing */
        float sgn = i ? 1.0f : -1.0f;
        vp_vec3 v = vp_body_vel(x->body);
        v.x += sgn * dx * k;
        v.z += sgn * dz * k;
        if (v.y < 4.0f) v.y = 4.0f;
        vp_body_set_vel(x->body, v);
        x->squash = -0.7f;
        x->grounded = 0;
    }
}

void props_bean_push(Bean *b, vp_vec3 v, int knock_over)
{
    if (!b->body || b->state == BEAN_OUT || b->fallen > 0) return;
    vp_body_set_vel(b->body, v);
    b->vel = v;
    b->grounded = 0;
    b->squash = -0.8f;
    if (knock_over) {
        float l = sqrtf(v.x * v.x + v.z * v.z) + 1e-4f;
        knock(b, v.x / l, v.z / l);
    }
}

/* The legs: a stride while running (by how far it has run), tucked in the
 * air, limp when it is down. The arms swing free. Every part falls at the
 * beans' own gravity. */
static void drive_limbs(const Bean *b, float dt)
{
    const int i = b->id;
    float speed = sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
    /* short, busy steps: a comic scurry, and the arms flailing in the air */
    S.stride[i] += (b->grounded ? speed * 4.0f : 22.0f) * dt;
    float amp = fminf(speed / RUN_SPEED, 1.0f);
    for (int k = 0; k < LIMBS; k++) {
        Limb *l = &S.limbs[i][k];
        if (!l->body) continue;
        vp_body_force(l->body, vp_v3(0, -LIMB_MASS * (GRAVITY - PROP_GRAVITY), 0));
        if (b->tumbling || b->fallen > 0 || b->state == BEAN_OUT) {
            vp_joint_motor(l->hinge, 0, 0);             /* limp */
            continue;
        }
        /* a positive turn about the hip's axis swings the foot back; each
         * arm swings against the leg on its side */
        float phase = (k == 1 || k == 2) ? 3.14159f : 0.0f;
        float target;
        if (k < 2) target = b->grounded ? sinf(S.stride[i] + phase) * amp * 1.15f : -0.7f;
        else       target = sinf(S.stride[i] + phase) * (b->grounded ? amp * 1.8f : 2.0f);
        float strength = k < 2 ? 160.0f : 70.0f;
        if (k == 3 && b->punch > 0) { target = -1.9f; strength = 160.0f; }     /* the punch: straight out ahead */
        vp_joint_motor(l->hinge, (target - vp_joint_position(l->hinge)) * 30.0f, strength);
    }
}

unsigned props_step(const Course *c, Bean *beans, int n, float t, float dt, unsigned *bounced)
{
    *bounced = 0;
    if (!S.world) return 0;
    S.t = t;
    /* the tiles under the beans: a fake one starts shaking, a real one is
     * seen to be safe; a fake one shaken long enough drops away */
    for (int i = 0; i < n && i < MAX_BEANS; i++) {
        const Bean *b = &beans[i];
        int k = b->ground.part;
        if (!b->grounded || b->state == BEAN_OUT || k < 0 || k >= c->nparts || c->parts[k].type != P_TILE) continue;
        if (!c->parts[k].variant) S.known[k] = 1;
        else if (!S.happened[k]) happen(k);
    }
    for (int k = 0; k < c->nparts; k++)
        if (c->parts[k].type == P_TILE && S.happened[k] && !S.gone[k] && S.t - S.when[k] > TILE_SHAKE)
            take_away(k);
    /* the course's moving parts: exactly where the clock puts them next */
    for (int i = 0; i < S.nmoving; i++) {
        const Part *p = &c->parts[S.moving[i].part];
        vp_body *body = S.moving[i].body;
        if (!body) continue;
        if (p->type == P_MOVER) {
            move_to(body, part_pos(p, t + dt), dt);
        } else if (p->type == P_SPINNER) {
            vp_vec3 at = vp_v3(p->c.x, p->c.y + p->h.y, p->c.z);
            vp_xf x = bar_place(at, spinner_angle(p, t));
            vp_body_set_place(body, &x);
            vp_body_set_spin(body, vp_v3(0, -p->w, 0));     /* its angle grows with w */
        } else if (p->type == P_PENDULUM) {
            move_to(body, pendulum_ball(p, t + dt), dt);
        } else if (p->type == P_ROLLER) {
            vp_vec3 ball;
            float run;
            if (roller_ball(p, t + dt, &ball, &run)) move_to(body, ball, dt);
            else { vp_body_set_pos(body, vp_v3(p->c.x, PARK_Y, p->c.z)); vp_body_set_vel(body, vp_v3(0, 0, 0)); }
        }
    }
    /* the beans' arms and legs */
    for (int i = 0; i < n && i < MAX_BEANS; i++)
        if (beans[i].body) drive_limbs(&beans[i], dt);
    /* seesaws come back level, slowly */
    for (int i = 0; i < S.nseesaws; i++)
        vp_joint_motor(S.seesaws[i].hinge, -1.2f * vp_joint_position(S.seesaws[i].hinge), 2500);
    /* big balls, dropped at the far ends */
    for (int i = 0; i < S.ndroppers; i++) {
        Dropper *dr = &S.droppers[i];
        dr->timer -= dt;
        if (dr->timer <= 0) {
            const Part *p = &c->parts[dr->part];
            dr->timer = p->w * (0.7f + 0.6f * rnd(&S.rng));
            drop_ball(p);
        }
    }

    vp_phys_step(S.world, dt);

    /* what fell off the world goes */
    for (int i = 0; i < MAX_BALLS; i++)
        if (S.balls[i].body && vp_body_pos(S.balls[i].body).y < GONE_Y) {
            vp_body_remove(S.balls[i].body);
            S.balls[i].body = NULL;
        }
    for (int i = 0; i < S.nblocks; i++)
        if (S.blocks[i].body && vp_body_pos(S.blocks[i].body).y < GONE_Y) {
            vp_body_remove(S.blocks[i].body);
            S.blocks[i].body = NULL;
        }

    /* the hazards (and the big balls, rolling hard) knock over the beans they hit */
    unsigned knocked = 0;
    unsigned paired[MAX_BEANS] = { 0 };             /* the pairs bounced this step: once each */
    int opened[8], nopened = 0;                     /* doors burst open: taken away after the contacts */
    for (int k = 0; k < vp_phys_contact_count(S.world); k++) {
        vp_contact ct;
        if (!vp_phys_contact(S.world, k, &ct)) continue;
        const Tag *ta = tag_of(ct.a), *tb = tag_of(ct.b);
        if (!ta || !tb) continue;
        if (ta->kind == K_BEAN && tb->kind == K_BEAN) {
            int i = ta->index, j = tb->index;
            if (i == j || i >= n || j >= n || (paired[i] >> j & 1)) continue;
            paired[i] |= 1u << j;
            paired[j] |= 1u << i;
            bounce(&beans[i], &beans[j], ct.speed);
            *bounced |= (1u << i) | (1u << j);
            continue;
        }
        const Tag *bean = ta->kind == K_BEAN ? ta : tb->kind == K_BEAN ? tb : NULL;
        const Tag *what = bean == ta ? tb : ta;
        vp_body *other = bean == ta ? ct.b : ct.a;
        if (!bean || what->kind == K_BEAN || bean->index >= n) continue;
        if (what->kind == K_DOOR) {
            int d = what->index;
            Bean *b = &beans[bean->index];
            if (S.gone[d] || b->state == BEAN_OUT) continue;
            if (!c->parts[d].variant) {             /* a real door: it bursts open */
                int dup = 0;
                for (int j = 0; j < nopened; j++) dup |= opened[j] == d;
                if (!dup && nopened < 8) { opened[nopened++] = d; happen(d); }
            } else {                                /* a fake one: bounced back */
                S.known[d] = 1;
                if (b->tumbling || b->fallen > 0) continue;
                vp_vec3 v = vp_body_vel(b->body);
                v.z = b->pos.z < c->parts[d].c.z ? -4.0f : 4.0f;
                if (v.y < 3.0f) v.y = 3.0f;
                vp_body_set_vel(b->body, v);
                b->squash = -0.7f;
                b->grounded = 0;
                *bounced |= 1u << bean->index;
            }
            continue;
        }
        if (what->kind == K_BUMPER) {               /* boing: thrown away from it */
            Bean *b = &beans[bean->index];
            if (b->state != BEAN_RACING || b->fallen > 0 || b->tumbling) continue;
            const Part *p = &c->parts[what->index];
            float dx = b->pos.x - p->c.x, dz = b->pos.z - p->c.z, l = sqrtf(dx * dx + dz * dz);
            if (l < 1e-3f) { dx = 0; dz = -1; l = 1; }
            vp_vec3 v = vp_v3(dx / l * 13.0f, 5.0f, dz / l * 13.0f);
            vp_body_set_vel(b->body, v);
            b->squash = -0.8f;
            b->grounded = 0;
            happen(what->index);
            *bounced |= 1u << bean->index;
            continue;
        }
        int hard = what->kind == K_HAZARD ||
                   (what->kind == K_BALL && vp_v3_length(vp_body_vel(other)) > 2.5f);
        if (!hard || bean->index >= n) continue;
        Bean *b = &beans[bean->index];
        if (b->state != BEAN_RACING || b->fallen > 0 || b->tumbling) continue;
        vp_vec3 away = vp_v3_sub(b->pos, vp_body_pos(other));
        float l = sqrtf(away.x * away.x + away.z * away.z) + 1e-4f;
        knock(b, away.x / l, away.z / l);
        knocked |= 1u << bean->index;
    }
    for (int j = 0; j < nopened; j++) take_away(opened[j]);
    return knocked;
}

/* ---- drawing -------------------------------------------------------------------------------- */

void props_draw(vp_dvg *d, vp3d_scene *scene, const vp3d_mesh *box, const vp3d_mesh *plank,
                const vp3d_mesh *ball, float near, float far)
{
    if (!S.world) return;
    for (int i = 0; i < S.nseesaws; i++) {
        const Seesaw *s = &S.seesaws[i];
        vp_xf place = vp_body_place(s->plank);
        if (place.t[2] + s->half.z < near || place.t[2] - s->half.z > far) continue;
        vp_xf drawn = vp_xf_mul2(place, vp_xf_scale(s->half.x, s->half.y, s->half.z));
        vp_color(d, 0, 255, 140);
        vp3d_scene_mesh(scene, plank, &drawn, VP3D_SOLID);     /* striped: its tilt shows */
        vp_xf post = vp_body_place(s->post);
        vp_xf pd = vp_xf_mul2(post, vp_xf_scale(s->post_half.x, s->post_half.y, s->post_half.z));
        vp_color(d, 120, 120, 160);
        vp3d_scene_mesh(scene, box, &pd, VP3D_SOLID);
    }
    vp_color(d, 255, 255, 255);
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!S.balls[i].body) continue;
        vp_xf place = vp_body_place(S.balls[i].body);
        if (place.t[2] < near || place.t[2] > far) continue;
        float r = S.balls[i].radius;
        vp_xf drawn = vp_xf_mul2(place, vp_xf_scale(r, r, r));
        vp3d_scene_mesh(scene, ball, &drawn, VP3D_SOLID);
    }
    vp_color(d, 255, 160, 40);
    for (int i = 0; i < S.nblocks; i++) {
        if (!S.blocks[i].body) continue;
        vp_xf place = vp_body_place(S.blocks[i].body);
        if (place.t[2] < near || place.t[2] > far) continue;
        vp_xf drawn = vp_xf_mul2(place, vp_xf_scale(S.blocks[i].half.x, S.blocks[i].half.y, S.blocks[i].half.z));
        vp3d_scene_mesh(scene, box, &drawn, VP3D_SOLID);
    }
}

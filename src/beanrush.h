/* beanrush.h — Bean Rush: a show of obstacle races for one player against a
 * crowd of computer beans, on the VectorPie SDK.
 *
 * World: meters, y up, the course running along +z (the camera looks down
 * it), x across. A course is a list of PARTS (platforms, moving platforms,
 * belts, spinning bars, pendulums, rolling balls...), each a function of the
 * race clock, so where a part is at any time is known in advance: the
 * computer beans use that to time their jumps.
 */
#ifndef BEANRUSH_H
#define BEANRUSH_H

#include <vectorpie/vp.h>

/* ---- the beans' physics ------------------------------------------------- */

#define BEAN_R        0.45f     /* radius */
#define BEAN_H        1.5f      /* height, feet to top */
#define RUN_SPEED     8.0f      /* m/s, top running speed */
#define ACCEL_GROUND  50.0f     /* m/s^2 */
#define ACCEL_AIR     14.0f
#define GRAVITY       28.0f
#define JUMP_SPEED    9.2f      /* up: about 1.5 m high, 0.66 s in the air */
#define DIVE_SPEED    11.0f
#define DIVE_TIME     0.75f     /* s lying down after a dive */
#define STEP_UP       0.45f     /* a ledge this high is walked onto */
#define KILL_Y        -14.0f    /* below this, a bean has fallen off */
#define RESPAWN_TIME  1.2f
#define BEAN_MASS     40.0f     /* kg: what it weighs on a seesaw, against a shove */
#define PROP_GRAVITY  16.0f     /* the physics world's (the beans get GRAVITY) */

#define MAX_BEANS     10
#define MAX_PARTS     320
#define MAX_CHECKS    32

/* ---- the course ------------------------------------------------------------ */

typedef enum {
    P_BOX,          /* a platform or a wall: c center, h half size */
    P_MOVER,        /* a box moving to and fro: c + axis * sin(w t + ph) */
    P_CONVEYOR,     /* a platform whose surface moves along z at `belt` m/s */
    P_SPINNER,      /* a bar turning about a post at c: half length h.x, at
                       h.y above c.y, angle w t + ph */
    P_PENDULUM,     /* a ball on a rope from the pivot c: rope h.y, ball h.x,
                       swinging across the course by h.z radians */
    P_ROLLER,       /* balls rolling toward the start down a lane: starting
                       at c (c.y the floor), radius h.x, run h.z long, w m/s */
    P_ARCH,         /* the finish arch (drawn only) */
    /* physical props (props.c): made in the physics world from these */
    P_SEESAW,       /* a plank tipping sideways on a hinge along z: c its top's
                       middle, h its half size; a post under it */
    P_BALLDROP,     /* big balls dropped at c (the far end), across h.x,
                       every w seconds, radius h.y, rolling toward the start */
    P_TOPPLE,       /* a wall of light blocks across: c its middle, h.x half
                       its width, h.y its height, h.z a block's half size */
    P_DOOR,         /* a door in a wall across the course: c, h a box; real
                       ones burst open when a bean runs into them, fake ones
                       (variant 1) stay shut */
    P_TILE,         /* a tile over the void: a floor; a fake one (variant 1)
                       shakes when stepped on, then drops away */
    P_BUMPER        /* a pinball bumper: c its foot, h.x its radius, h.y half
                       its height (h.z = h.x); it bounces beans away */
} PartType;

typedef struct {
    PartType type;
    vp_vec3  c, h;
    vp_vec3  axis;          /* P_MOVER: direction times amplitude */
    float    w, ph;
    float    belt;          /* P_CONVEYOR */
    int      variant;       /* P_DOOR, P_TILE: fake; P_ROLLER: fruit */
    int      floor;         /* beans stand on it */
    int      solid;         /* beans bump into its sides */
    uint8_t  r, g, b;
} Part;

typedef struct {
    float z;                /* reached once a bean stands past it */
    float x0, x1;           /* where to put a bean back, across */
    float y;
} Checkpoint;

typedef struct {
    const char *name;
    Part       parts[MAX_PARTS];
    int        nparts;
    Checkpoint checks[MAX_CHECKS];
    int        nchecks;
    float      finish_z;    /* crossing it finishes the race */
    float      length;      /* the far end */
} Course;

/* What a bean stands on. */
typedef struct {
    int      part;          /* the course's part, -1: none (or a prop) */
    vp_body *body;          /* a physics prop (a seesaw...), or NULL */
    float    top;
    vp_vec3  carry;         /* the surface's own movement, m/s */
    vp_vec3  normal;        /* which way it faces (tilted on a seesaw) */
} Support;

/* Floors that are not the course's own parts: the physics props (props.c),
 * found where they are now. Set by the game; NULL: none. */
typedef int (*ExtraFloor)(float x, float z, float y, Support *s);
extern ExtraFloor g_extra_floor;

/* The courses: a show draws one for each of its rounds from those made for
 * that round (course_round: 0, 1, or 2, the final). `seed` sets what changes
 * from race to race: which doors are fake, the safe way over the tiles. */
int  course_count(void);
int  course_round(int id);
const char *course_name(int id);
void course_build(Course *c, int id, unsigned seed);

/* Where part p is at time t (its center; for a spinner the bar's angle in
 * *angle, for a roller its run so far in *run, -1 when none is rolling). */
vp_vec3 part_pos(const Part *p, float t);
vp_vec3 part_vel(const Part *p, float t);
float   spinner_angle(const Part *p, float t);
float   pendulum_angle(const Part *p, float t);
int     roller_ball(const Part *p, float t, vp_vec3 *center, float *run);

/* The highest floor under (x, z) at time t that a bean whose feet are at y
 * can stand on. 0 when there is none. */
int  course_floor(const Course *c, float x, float z, float y, float t, Support *s);
/* A hazard where a bean at pos (feet) would be at time t: 1, and the push it
 * would give in *knock. For the computer's beans to see one coming (the
 * hazards themselves are bodies in the physics world, props.c). */
int  course_hazard(const Course *c, vp_vec3 pos, float t, vp_vec3 *knock);

/* ---- the beans --------------------------------------------------------------- */

enum { BEAN_RACING, BEAN_FINISHED, BEAN_OUT };

typedef struct {
    int      state;
    int      human;
    int      id;            /* its number in the physics world (props.c) */
    vp_body *body;          /* its capsule there; its arms and legs hang from it */
    int      tumbling;      /* knocked over: turning freely until it gets up */
    vp_vec3  pos, vel;
    float    heading;       /* the way it faces: 0 = +z */
    int      grounded;
    Support  ground;
    float    stun;          /* s knocked over, no control */
    float    dive;          /* s left lying after a dive */
    float    fallen;        /* s since it fell off (0: it did not) */
    float    squash;        /* jump / landing bounce, drawn */
    int      check;         /* last checkpoint reached */
    float    finish_time;
    int      place;         /* 1.. when finished */
    uint8_t  r, g, b;
    /* the computer's */
    float    skill;         /* 0.8 .. 1: its speed and its timing */
    float    lane;          /* -1 .. 1 across the course */
    float    hold;          /* s it waits for something to pass */
    float    think;         /* s until it next rolls the dice */
    int      sloppy;        /* this time round, it misjudges */
    float    stuck;         /* s without progress */
    float    best_z;
    /* its mood: bumped by others it gets annoyed, then furious */
    float    temper;        /* 0 calm .. 1 furious */
    int      grudge;        /* the bean that pushed it most (-1: none) */
    float    revenge;       /* s left going after its grudge (computer's beans) */
    float    punch;         /* s left of a punch's swing (drawn, its arm) */
    float    cool;          /* s before it can punch again */
} Bean;

/* The controls a bean is given for one step: a direction on the ground
 * (x across, z along the course, length up to 1), jump and dive presses. */
typedef struct {
    float x, z;
    int   jump, dive;
    int   punch;            /* punch the nearest bean, or shove it over an edge */
} BeanInput;

void bean_place(Bean *b, const Course *c, int slot, int count);
/* Before the physics step: its controls steer its body. After it: where
 * the body went, what it stands on, the checkpoints, the finish. */
void bean_control(Bean *b, const BeanInput *in, const Course *c, float t, float dt, int *events);
void bean_sync(Bean *b, const Course *c, float t, float dt, int *events);
void bean_ai(Bean *b, const Course *c, float t, BeanInput *out, unsigned *rng);
float bean_progress(const Bean *b);

/* events bean_control and bean_sync report (bits) */
#define EV_JUMP    1
#define EV_DIVE    2
#define EV_HIT     4
#define EV_FALL    8
#define EV_LAND    16
#define EV_PUNCH   32
#define EV_SHOVE   64
#define EV_WHIFF   128

/* Moods (beans.c): another bean bumped `victim` (`by` its index), this
 * hard. Each step: tempers cool; a furious computer bean sets off after its
 * grudge. A punch (or a shove, at an edge) at the nearest bean in
 * front; returns its events. The computer's revenge: chase and punch. */
void bean_annoy(Bean *victim, int by, float amount);
void bean_mood(Bean *b, Bean *beans, int n, float dt);
int  bean_strike(Bean *b, Bean *beans, int n, const Course *c, float t);
void bean_revenge(Bean *b, const Bean *beans, BeanInput *out);

/* ---- drawing ------------------------------------------------------------------- */

int  render_init(void);
void render_free(void);
/* hide(scene): adds what hides the world (the big lettering's screens)
 * before the scene is drawn; may be NULL. */
typedef void (*RenderHide)(vp3d_scene *scene);
void render_world(vp_dvg *d, const vp3d_view *view, const Course *c, float t,
                  const Bean *beans, int n, int marked, int crowned, RenderHide hide);

/* ---- the physical props (props.c) --------------------------------------------- */

/* Make the race's physics world: the course (its platforms, moving parts
 * and hazards), its props, and a body for each bean (beans[i].body). It
 * also sets g_extra_floor, for the computer's beans to see the props. */
void props_build(const Course *c, Bean *beans, int n, unsigned seed);
void props_free(void);
/* The physics step, after the beans' controls: returns the beans knocked
 * over (bit i: bean i), now tumbling; *bounced the beans that bumped into
 * each other (sprung apart). */
unsigned props_step(const Course *c, Bean *beans, int n, float t, float dt, unsigned *bounced);
/* A bean's body: steered by its velocity (bean_control), read back
 * (bean_sync), put upright at its place (a respawn, getting up; parked out
 * of the way while it has fallen or is out), what is under it. */
void props_bean_drive(Bean *b, float dt);
void props_bean_read(Bean *b);
void props_bean_put(Bean *b);
int  props_bean_ground(const Bean *b, Support *s);
/* Thrown: its body given velocity v; knocked over too (a punch), or only
 * sent flying (a shove). */
void props_bean_push(Bean *b, vp_vec3 v, int knock_over);
/* A dive: it pitches forward onto its belly, limp, until it gets up. */
void props_bean_dive(Bean *b);
/* What is straight below the bean, for its shadow: 1 and its height. */
int  props_bean_shadow(const Bean *b, float *floor_y);
/* Limb k (0, 1 the legs, 2, 3 the arms) as it is: its joint, its tip, how fat. */
int  props_limb(const Bean *b, int k, vp_vec3 *joint, vp_vec3 *tip, float *radius);
/* What has happened to a part in the physics world: a door burst open or a
 * tile dropped away (gone); a door found to be fake, a tile found to be
 * safe (known: the beans have seen it); seconds since it opened, started
 * shaking or bounced a bean (-1: never). */
int   props_part_gone(int part);
int   props_part_known(int part);
float props_part_age(int part);
void props_draw(vp_dvg *d, vp3d_scene *scene, const vp3d_mesh *box, const vp3d_mesh *plank,
                const vp3d_mesh *ball, float near, float far);

/* ---- a small random generator (the same seed, the same race) --------------- */

float rnd(unsigned *state);     /* 0 .. 1 */

/* music.c: the packed music, data/music.dat */
int            music_open(const char *path);
unsigned char *music_track(const char *name, size_t *size);   /* malloc'd Ogg, or NULL */
void           music_close(void);

#endif

/* render.c — the course and the beans, drawn through a vp3d scene so that
 * platforms and beans hide what is behind them.
 */
#include "beanrush.h"

#include <math.h>
#include <stdio.h>

static vp3d_mesh  *g_box;       /* a cube, corners at +-1 */
static vp3d_mesh  *g_bean;      /* feet at 0, BEAN_H tall, its front +z */
static vp3d_mesh  *g_eyes;      /* on its front: drawn with it, in white */
static vp3d_mesh  *g_ball;      /* radius 1, a band round it to see it roll */
static vp3d_mesh  *g_leg, *g_arm;   /* a limb and its round end, one shape: from the joint (0) down y */
/* Textures (vp3d textures: line drawings on faces) */
static vp3d_texture *g_floor_tex;           /* the floors: Cyber Sled's grid, cartoons in it */
static vp3d_texture *g_plank_tex;           /* the seesaws' planks: stripes across */
static vp3d_mesh    *g_floor;               /* a cube, its top textured: the platforms */
static vp3d_mesh    *g_plank;               /* a cube, its top striped */
static vp3d_scene *g_scene;

/* A placement from its axes (each scaled) and its origin. */
static vp_xf xf_basis(vp_vec3 o, vp_vec3 ex, vp_vec3 ey, vp_vec3 ez)
{
    vp_xf x;
    x.m[0] = ex.x; x.m[1] = ey.x; x.m[2] = ez.x;
    x.m[3] = ex.y; x.m[4] = ey.y; x.m[5] = ez.y;
    x.m[6] = ex.z; x.m[7] = ey.z; x.m[8] = ez.z;
    x.t[0] = o.x; x.t[1] = o.y; x.t[2] = o.z;
    return x;
}

static vp_xf xf_box(vp_vec3 c, vp_vec3 h)
{
    return xf_basis(c, vp_v3(h.x, 0, 0), vp_v3(0, h.y, 0), vp_v3(0, 0, h.z));
}

/* A cube, corners at +-1; `top`: a texture on its top face, world mapped,
 * one tile `tile` meters across. */
static vp3d_mesh *new_box(const vp3d_texture *top, float tile)
{
    vp3d_mesh *m;
    if (vp3d_mesh_create(&m) != VP_OK) return NULL;
    for (int i = 0; i < 8; i++)
        vp3d_mesh_vertex(m, (i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    static const int faces[6][4] = {
        { 0, 2, 3, 1 }, { 4, 5, 7, 6 },     /* z- , z+ */
        { 0, 1, 5, 4 }, { 2, 6, 7, 3 },     /* y- , y+ */
        { 0, 4, 6, 2 }, { 1, 3, 7, 5 },     /* x- , x+ */
    };
    for (int f = 0; f < 6; f++) vp3d_mesh_face(m, faces[f], 4);
    if (top) {
        vp3d_mesh_face_texture(m, 3, top);
        vp3d_mesh_texture_world(m, tile);
    }
    vp3d_mesh_finish(m);
    return m;
}

/* A bean's body: rings of 10 round a vertical axis, round at the bottom and
 * top, on short legs (drawn apart: draw_limbs). A ring round its waist is a
 * lone line just outside it: the scene hides its far side, so the body reads
 * as round. Its eyes are a mesh of their own (new_eyes), drawn white. */
#define BODY_Y 0.36f                /* the body's bottom: the legs below */
static vp3d_mesh *new_bean(void)
{
    static const float ring_y[] = { 0.42f, 0.56f, 0.80f, 1.07f, 1.30f, 1.44f };
    static const float ring_r[] = { 0.30f, 0.42f, 0.46f, 0.44f, 0.35f, 0.19f };
    enum { SIDES = 10, RINGS = 6 };
    vp3d_mesh *m;
    if (vp3d_mesh_create(&m) != VP_OK) return NULL;
    int bottom = vp3d_mesh_vertex(m, 0, BODY_Y, 0);
    int first = -1;
    for (int r = 0; r < RINGS; r++)
        for (int s = 0; s < SIDES; s++) {
            float a = 6.28318f * s / SIDES;
            int v = vp3d_mesh_vertex(m, ring_r[r] * sinf(a), ring_y[r], ring_r[r] * cosf(a));
            if (first < 0) first = v;
        }
    int top = vp3d_mesh_vertex(m, 0, BEAN_H + 0.02f, 0);
    for (int s = 0; s < SIDES; s++) {
        int s1 = (s + 1) % SIDES;
        int f[3] = { bottom, first + s1, first + s };
        vp3d_mesh_face(m, f, 3);
        for (int r = 0; r + 1 < RINGS; r++) {
            int q[4] = { first + r * SIDES + s, first + r * SIDES + s1,
                         first + (r + 1) * SIDES + s1, first + (r + 1) * SIDES + s };
            vp3d_mesh_face(m, q, 4);
        }
        int t[3] = { top, first + (RINGS - 1) * SIDES + s, first + (RINGS - 1) * SIDES + s1 };
        vp3d_mesh_face(m, t, 3);
    }
    /* a ring round its waist: its far side hidden, it shows the body round */
    int w[16];
    for (int i = 0; i < 16; i++) {
        float a = 6.28318f * i / 16;
        w[i] = vp3d_mesh_vertex(m, 0.475f * sinf(a), 0.80f, 0.475f * cosf(a));
    }
    for (int i = 0; i < 16; i++) vp3d_mesh_line(m, w[i], w[(i + 1) % 16]);
    vp3d_mesh_set_crease(m, 60);
    vp3d_mesh_finish(m);
    return m;
}

/* Its eyes: two tall ovals side by side on the front of the body, a big one
 * and a small one (lines just outside the surface, hidden when it turns
 * away). */
static vp3d_mesh *new_eyes(void)
{
    static const float eye[2][4] = { { -0.075f, 1.12f, 0.095f, 0.14f },     /* x, y, half width, half height */
                                     { 0.085f, 1.12f, 0.045f, 0.068f } };
    enum { N = 12 };
    vp3d_mesh *m;
    if (vp3d_mesh_create(&m) != VP_OK) return NULL;
    for (int e = 0; e < 2; e++) {
        int v[N];
        for (int i = 0; i < N; i++) {
            float a = 6.28318f * i / N;
            float x = eye[e][0] + eye[e][2] * cosf(a), y = eye[e][1] + eye[e][3] * sinf(a);
            /* the body's radius at that height (between its rings at 1.07 and 1.30 m) */
            float r = 0.44f + (y - 1.07f) / 0.23f * (0.35f - 0.44f);
            v[i] = vp3d_mesh_vertex(m, x, y, sqrtf(fmaxf(r * r - x * x, 0.0f)) + 0.012f);
        }
        for (int i = 0; i < N; i++) vp3d_mesh_line(m, v[i], v[(i + 1) % N]);
    }
    vp3d_mesh_finish(m);
    return m;
}

static vp3d_mesh *new_ball(int bands)
{
    enum { SIDES = 12, RINGS = 7 };
    vp3d_mesh *m;
    if (vp3d_mesh_create(&m) != VP_OK) return NULL;
    int south = vp3d_mesh_vertex(m, 0, -1, 0);
    int first = -1;
    for (int r = 1; r < RINGS; r++) {
        float la = -1.5708f + 3.14159f * r / RINGS;
        for (int s = 0; s < SIDES; s++) {
            float a = 6.28318f * s / SIDES;
            int v = vp3d_mesh_vertex(m, cosf(la) * sinf(a), sinf(la), cosf(la) * cosf(a));
            if (first < 0) first = v;
        }
    }
    int north = vp3d_mesh_vertex(m, 0, 1, 0);
    for (int s = 0; s < SIDES; s++) {
        int s1 = (s + 1) % SIDES;
        int f[3] = { south, first + s1, first + s };
        vp3d_mesh_face(m, f, 3);
        for (int r = 0; r + 1 < RINGS - 1; r++) {
            int q[4] = { first + r * SIDES + s, first + r * SIDES + s1,
                         first + (r + 1) * SIDES + s1, first + (r + 1) * SIDES + s };
            vp3d_mesh_face(m, q, 4);
        }
        int t[3] = { north, first + (RINGS - 2) * SIDES + s, first + (RINGS - 2) * SIDES + s1 };
        vp3d_mesh_face(m, t, 3);
    }
    /* three rings round it, square to each other: the scene hides their far
     * halves, so it reads as a ball; they turn as it rolls */
    for (int k = 0; k < (bands ? 3 : 0); k++) {
        int band[16];
        for (int i = 0; i < 16; i++) {
            float a = 6.28318f * i / 16, c = 1.02f * cosf(a), s = 1.02f * sinf(a);
            band[i] = k == 0 ? vp3d_mesh_vertex(m, c, s, 0)
                    : k == 1 ? vp3d_mesh_vertex(m, 0, c, s)
                             : vp3d_mesh_vertex(m, c, 0, s);
        }
        for (int i = 0; i < 16; i++) vp3d_mesh_line(m, band[i], band[(i + 1) % 16]);
    }
    vp3d_mesh_set_crease(m, 180);                   /* its outline and the band */
    vp3d_mesh_finish(m);
    return m;
}

/* ---- the floors' icons: flat cartoons, x across and y ahead, about 1 wide ---- */

typedef struct { int n; float pts[24][2]; int closed; } Stroke;

static void icon_stroke(vp3d_texture *t, const Stroke *k, float cu, float cv, float size)
{
    float uv[48];
    for (int i = 0; i < k->n; i++) {
        uv[i * 2] = cu + k->pts[i][0] * size;
        uv[i * 2 + 1] = cv + k->pts[i][1] * size;
    }
    vp3d_texture_path(t, uv, k->n, k->closed);
}

static void ring(Stroke *k, float x, float y, float r, int n)
{
    k->n = n;
    k->closed = 1;
    for (int i = 0; i < n; i++) {
        float a = 6.28318f * i / n;
        k->pts[i][0] = x + r * cosf(a);
        k->pts[i][1] = y + r * sinf(a);
    }
}

/* icon 0 a polka dot, 1 a star, 2 a heart, 3 a smiley, 4 a little bean */
static void add_icon(vp3d_texture *s, int which, float cx, float cz, float size)
{
    Stroke k;
    switch (which) {
    case 0:
        ring(&k, 0, 0, 0.32f, 8);
        icon_stroke(s, &k, cx, cz, size);
        ring(&k, 0, 0, 0.14f, 6);
        icon_stroke(s, &k, cx, cz, size);
        break;
    case 1:
        k.n = 10; k.closed = 1;
        for (int i = 0; i < 10; i++) {
            float a = 1.5708f + 6.28318f * i / 10, r = (i & 1) ? 0.19f : 0.46f;
            k.pts[i][0] = r * cosf(a);
            k.pts[i][1] = r * sinf(a);
        }
        icon_stroke(s, &k, cx, cz, size);
        break;
    case 2: {
        static const float h[12][2] = {
            { 0, -0.42f }, { -0.36f, -0.05f }, { -0.42f, 0.14f }, { -0.36f, 0.30f }, { -0.20f, 0.36f },
            { -0.06f, 0.30f }, { 0, 0.20f }, { 0.06f, 0.30f }, { 0.20f, 0.36f }, { 0.36f, 0.30f },
            { 0.42f, 0.14f }, { 0.36f, -0.05f } };
        k.n = 12; k.closed = 1;
        for (int i = 0; i < 12; i++) { k.pts[i][0] = h[i][0]; k.pts[i][1] = h[i][1]; }
        icon_stroke(s, &k, cx, cz, size);
        break;
    }
    case 3:
        ring(&k, 0, 0, 0.42f, 10);
        icon_stroke(s, &k, cx, cz, size);
        ring(&k, -0.15f, 0.13f, 0.05f, 4);
        icon_stroke(s, &k, cx, cz, size);
        ring(&k, 0.15f, 0.13f, 0.05f, 4);
        icon_stroke(s, &k, cx, cz, size);
        k.n = 5; k.closed = 0;                      /* the smile */
        for (int i = 0; i < 5; i++) {
            float a = 3.5f + 2.42f * i / 4;
            k.pts[i][0] = 0.25f * cosf(a);
            k.pts[i][1] = 0.02f + 0.25f * sinf(a);
        }
        icon_stroke(s, &k, cx, cz, size);
        break;
    default: {
        static const float b[10][2] = {
            { -0.16f, -0.40f }, { 0.16f, -0.40f }, { 0.26f, -0.20f }, { 0.27f, 0.12f }, { 0.20f, 0.34f },
            { 0, 0.44f }, { -0.20f, 0.34f }, { -0.27f, 0.12f }, { -0.26f, -0.20f }, { -0.16f, -0.40f } };
        k.n = 9; k.closed = 1;
        for (int i = 0; i < 9; i++) { k.pts[i][0] = b[i][0]; k.pts[i][1] = b[i][1]; }
        icon_stroke(s, &k, cx, cz, size);
        k.n = 2; k.closed = 0;                      /* its visor */
        k.pts[0][0] = -0.13f; k.pts[0][1] = 0.18f; k.pts[1][0] = 0.13f; k.pts[1][1] = 0.18f;
        icon_stroke(s, &k, cx, cz, size);
        break;
    }
    }
}

/* ---- textures -------------------------------------------------------------------- */

/* The floors': an 8 m tile of 2 m squares (Cyber Sled's grid), a cartoon in
 * every other square, a mix of the five. */
static vp3d_texture *floor_texture(void)
{
    vp3d_texture *t;
    if (vp3d_texture_create(&t) != VP_OK) return NULL;
    vp3d_texture_set_dim(t, 0.6f);
    for (int k = 0; k < 4; k++) {
        vp3d_texture_line(t, k / 4.0f, 0, k / 4.0f, 1);
        vp3d_texture_line(t, 0, k / 4.0f, 1, k / 4.0f);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (!((i + j) & 1)) add_icon(t, (i * 3 + j * 2) % 5, (i + 0.5f) / 4, (j + 0.5f) / 4, 1.3f / 8.0f);
    return t;
}

/* The seesaws': a stripe across (every 1.5 m, by world mapping). */
static vp3d_texture *plank_texture(void)
{
    vp3d_texture *t;
    if (vp3d_texture_create(&t) != VP_OK) return NULL;
    vp3d_texture_set_dim(t, 0.45f);
    vp3d_texture_line(t, 0, 0.5f, 1, 0.5f);
    return t;
}

/* A limb: a cone from its joint at the top (y 0, thin) widening down to its
 * tip (y -len), closed there by a flattened dome `e` across (a mitt, a foot:
 * a broad pad). Drawn as its outline: one contour. */
static vp3d_mesh *new_limb(float len, float e)
{
    enum { SIDES = 10, CAP = 3 };
    const float r0 = 0.05f;                         /* at the joint */
    const float h = 0.4f * e;                       /* the dome's depth: flat */
    float prof[2 + CAP][2];
    prof[0][0] = 0;              prof[0][1] = r0;
    prof[1][0] = -len + h;       prof[1][1] = e;     /* the cone's wide end, where the dome starts */
    for (int i = 1; i <= CAP; i++) {               /* the dome, down to the tip */
        float a = 1.5708f * i / (CAP + 1);
        prof[1 + i][0] = -len + h - h * sinf(a);
        prof[1 + i][1] = e * cosf(a);
    }
    const int N = 2 + CAP;
    vp3d_mesh *m;
    if (vp3d_mesh_create(&m) != VP_OK) return NULL;
    int top = vp3d_mesh_vertex(m, 0, 0.6f * r0, 0), first = -1;
    for (int i = 0; i < N; i++)
        for (int k = 0; k < SIDES; k++) {
            float ang = 6.28318f * k / SIDES;
            int v = vp3d_mesh_vertex(m, prof[i][1] * cosf(ang), prof[i][0], prof[i][1] * sinf(ang));
            if (first < 0) first = v;
        }
    int bottom = vp3d_mesh_vertex(m, 0, -len, 0);
    for (int k = 0; k < SIDES; k++) {
        int k1 = (k + 1) % SIDES;
        int t[3] = { top, first + k1, first + k };
        vp3d_mesh_face(m, t, 3);
        for (int i = 0; i + 1 < N; i++) {
            int q[4] = { first + i * SIDES + k, first + i * SIDES + k1,
                         first + (i + 1) * SIDES + k1, first + (i + 1) * SIDES + k };
            vp3d_mesh_face(m, q, 4);
        }
        int b[3] = { bottom, first + (N - 1) * SIDES + k, first + (N - 1) * SIDES + k1 };
        vp3d_mesh_face(m, b, 3);
    }
    vp3d_mesh_set_crease(m, 180);                   /* the outline only */
    vp3d_mesh_finish(m);
    return m;
}

int render_init(void)
{
    g_floor_tex = floor_texture();
    g_plank_tex = plank_texture();
    g_box = new_box(NULL, 0);
    g_floor = new_box(g_floor_tex, 8.0f);
    g_plank = new_box(g_plank_tex, 1.5f);
    g_bean = new_bean();
    g_eyes = new_eyes();
    g_ball = new_ball(1);
    g_leg = new_limb(0.38f, 0.13f);
    g_arm = new_limb(0.38f, 0.115f);
    if (!g_box || !g_floor || !g_plank || !g_bean || !g_ball || !g_leg || !g_arm) return 0;
    if (vp3d_scene_create(&g_scene) != VP_OK) return 0;
    vp3d_scene_set_contact(g_scene, 0.06f);         /* things on the floor sink in a little */
    return 1;
}

void render_free(void)
{
    vp3d_mesh_free(g_box);
    vp3d_mesh_free(g_floor);
    vp3d_mesh_free(g_plank);
    vp3d_texture_free(g_floor_tex);
    vp3d_texture_free(g_plank_tex);
    vp3d_mesh_free(g_bean);
    vp3d_mesh_free(g_eyes);
    vp3d_mesh_free(g_ball);
    vp3d_mesh_free(g_leg);
    vp3d_mesh_free(g_arm);
    vp3d_scene_free(g_scene);
}

/* ---- the course ------------------------------------------------------------------ */

static void part_extent(const Part *p, float *z0, float *z1)
{
    switch (p->type) {
    case P_ROLLER:   *z0 = p->c.z - p->h.z - 3; *z1 = p->c.z + 3; break;
    case P_SPINNER:  *z0 = p->c.z - p->h.x; *z1 = p->c.z + p->h.x; break;
    case P_PENDULUM: *z0 = p->c.z - 2; *z1 = p->c.z + 2; break;
    default:         *z0 = p->c.z - p->h.z; *z1 = p->c.z + p->h.z; break;
    }
}

/* The floors: their markings are a texture (the grid, the cartoons:
 * floor_texture); here the line just inside their edges, so where a floor
 * ends always shows, and a line under the top edge along each long side,
 * which makes a platform a thick slab. */
static void floor_edges(vp_dvg *d, const Part *p, vp_vec3 c)
{
    vp3d_scene *s = g_scene;
    const float top = c.y + p->h.y + 0.01f;
    const float x0 = c.x - p->h.x, x1 = c.x + p->h.x, z0 = c.z - p->h.z, z1 = c.z + p->h.z;
    const float in = p->h.x > 2.5f ? 0.45f : 0.25f;
    const float ex0 = x0 + in, ex1 = x1 - in, ez0 = z0 + in, ez1 = z1 - in;
    if (ex1 <= ex0 || ez1 <= ez0) return;
    vp_color(d, p->r / 2, p->g / 2, p->b / 2);
    vp3d_scene_line(s, vp_v3(ex0, top, ez0), vp_v3(ex1, top, ez0));
    vp3d_scene_line(s, vp_v3(ex1, top, ez0), vp_v3(ex1, top, ez1));
    vp3d_scene_line(s, vp_v3(ex1, top, ez1), vp_v3(ex0, top, ez1));
    vp3d_scene_line(s, vp_v3(ex0, top, ez1), vp_v3(ex0, top, ez0));
    float side = top - 0.01f - fminf(0.3f, p->h.y);
    vp3d_scene_line(s, vp_v3(x0 - 0.01f, side, z0), vp_v3(x0 - 0.01f, side, z1));
    vp3d_scene_line(s, vp_v3(x1 + 0.01f, side, z0), vp_v3(x1 + 0.01f, side, z1));
}

static void draw_part(vp_dvg *d, const Part *p, const Part *c0, float t)
{
    vp3d_scene *s = g_scene;
    vp_color(d, p->r, p->g, p->b);
    switch (p->type) {
    case P_BOX:
    case P_MOVER: {
        vp_vec3 c = part_pos(p, t);
        vp_xf x = xf_box(c, p->h);
        vp3d_scene_mesh(s, p->floor ? g_floor : g_box, &x, VP3D_SOLID);
        if (p->floor && p->h.z > 0.6f) floor_edges(d, p, c);
        break;
    }
    case P_CONVEYOR: {
        vp_xf x = xf_box(p->c, p->h);
        vp3d_scene_mesh(s, g_box, &x, VP3D_SOLID);
        /* the belt's slats, running with it */
        float top = p->c.y + p->h.y + 0.01f, gap = 1.6f;
        float off = fmodf(p->belt * t, gap);
        if (off < 0) off += gap;
        vp_color(d, p->r / 2, p->g / 2, p->b / 2);
        for (float z = p->c.z - p->h.z + off; z < p->c.z + p->h.z; z += gap)
            vp3d_scene_line(s, vp_v3(p->c.x - p->h.x + 0.3f, top, z), vp_v3(p->c.x + p->h.x - 0.3f, top, z));
        break;
    }
    case P_SPINNER: {
        float a = spinner_angle(p, t);
        vp_vec3 u = vp_v3(cosf(a), 0, sinf(a)), w = vp_v3(-sinf(a), 0, cosf(a));
        vp_xf x = xf_basis(vp_v3(p->c.x, p->c.y + p->h.y, p->c.z), vp_v3_scale(u, p->h.x),
                           vp_v3(0, 0.2f, 0), vp_v3_scale(w, 0.2f));
        vp3d_scene_mesh(s, g_box, &x, VP3D_SOLID);
        break;
    }
    case P_PENDULUM: {
        float a = pendulum_angle(p, t);
        vp_vec3 ball = vp_v3(p->c.x + sinf(a) * p->h.y, p->c.y - cosf(a) * p->h.y, p->c.z);
        vp_color(d, 160, 160, 200);
        vp3d_scene_line(s, p->c, ball);
        vp_color(d, p->r, p->g, p->b);
        vp_xf x = xf_basis(ball, vp_v3(p->h.x, 0, 0), vp_v3(0, p->h.x, 0), vp_v3(0, 0, p->h.x));
        vp3d_scene_mesh(s, g_ball, &x, VP3D_SOLID);
        break;
    }
    case P_ROLLER: {
        vp_vec3 ball;
        float run;
        if (!roller_ball(p, t, &ball, &run)) break;
        float spin = run / p->h.x;                  /* rolling toward -z: about x */
        float r = p->h.x;
        vp_xf x = xf_basis(ball, vp_v3(r, 0, 0), vp_v3(0, r * cosf(spin), -r * sinf(spin)),
                           vp_v3(0, r * sinf(spin), r * cosf(spin)));     /* its top going toward -z */
        vp3d_scene_mesh(s, g_ball, &x, VP3D_SOLID);
        break;
    }
    case P_DOOR: {
        float age = props_part_age((int)(p - c0));
        float hz = p->c.z - p->h.z, y0 = p->c.y - p->h.y;
        if (props_part_gone((int)(p - c0))) {
            /* burst open: falling forward about its foot, then lying flat */
            float a = fminf(age / 0.3f, 1.0f) * 1.5708f;
            vp_vec3 up = vp_v3(0, cosf(a), sinf(a)), fwd = vp_v3(0, -sinf(a), cosf(a));
            vp_vec3 mid = vp_v3(p->c.x, y0 + p->h.y * cosf(a) + p->h.z * sinf(a), p->c.z + p->h.y * sinf(a));
            vp_xf x = xf_basis(mid, vp_v3(p->h.x, 0, 0), vp_v3_scale(up, p->h.y), vp_v3_scale(fwd, p->h.z));
            vp3d_scene_mesh(s, g_box, &x, VP3D_SOLID);
            break;
        }
        vp_xf x = xf_box(p->c, p->h);
        vp3d_scene_mesh(s, g_box, &x, VP3D_SOLID);
        /* its panel and knob, on the side the beans come from */
        vp_color(d, p->r / 2, p->g / 2, p->b / 2);
        float px = p->h.x * 0.7f, z = hz - 0.01f;
        float pt = y0 + p->h.y * 1.8f, pb = y0 + p->h.y * 1.1f;
        vp3d_scene_line(s, vp_v3(p->c.x - px, pb, z), vp_v3(p->c.x + px, pb, z));
        vp3d_scene_line(s, vp_v3(p->c.x + px, pb, z), vp_v3(p->c.x + px, pt, z));
        vp3d_scene_line(s, vp_v3(p->c.x + px, pt, z), vp_v3(p->c.x - px, pt, z));
        vp3d_scene_line(s, vp_v3(p->c.x - px, pt, z), vp_v3(p->c.x - px, pb, z));
        vp_color(d, 255, 230, 120);
        vp_vec3 prev = vp_v3(0, 0, 0), knob = vp_v3(p->c.x + p->h.x * 0.6f, y0 + p->h.y * 0.95f, z);
        for (int i = 0; i <= 6; i++) {
            float k = 6.28318f * i / 6;
            vp_vec3 q = vp_v3(knob.x + 0.12f * cosf(k), knob.y + 0.12f * sinf(k), z);
            if (i) vp3d_scene_line(s, prev, q);
            prev = q;
        }
        break;
    }
    case P_TILE: {
        float age = props_part_age((int)(p - c0));
        vp_vec3 c = p->c;
        if (props_part_gone((int)(p - c0))) {       /* dropping away */
            float fall = age - 0.6f;
            if (fall > 1.6f) break;
            c.y -= 0.5f * 16.0f * fall * fall;
        } else if (age >= 0) {                      /* shaking */
            c.x += 0.07f * sinf(age * 70.0f);
            c.z += 0.05f * cosf(age * 55.0f);
        }
        vp_xf x = xf_box(c, p->h);
        vp3d_scene_mesh(s, g_box, &x, VP3D_SOLID);
        break;
    }
    case P_BUMPER: {
        float age = props_part_age((int)(p - c0));
        float pop = (age >= 0 && age < 0.25f) ? 1.0f + 0.25f * (1.0f - age / 0.25f) : 1.0f;
        if (pop > 1.0f) vp_color(d, 255, 255, 255);
        float r = p->h.x * pop;
        vp_vec3 c = vp_v3(p->c.x, p->c.y + p->h.y, p->c.z);
        vp_xf x = xf_basis(c, vp_v3(r, 0, 0), vp_v3(0, p->h.y * pop, 0), vp_v3(0, 0, r));
        vp3d_scene_mesh(s, g_ball, &x, VP3D_SOLID);
        /* its ring, round the middle */
        vp_color(d, 255, 230, 60);
        vp_vec3 prev = vp_v3(0, 0, 0);
        for (int i = 0; i <= 12; i++) {
            float k = 6.28318f * i / 12;
            vp_vec3 q = vp_v3(c.x + r * 1.04f * cosf(k), c.y, c.z + r * 1.04f * sinf(k));
            if (i) vp3d_scene_line(s, prev, q);
            prev = q;
        }
        break;
    }
    case P_ARCH: {
        float hx = p->h.x, hy = p->h.y, z = p->c.z;
        vp_vec3 post = vp_v3(0.35f, hy / 2, 0.35f);
        vp_xf l = xf_box(vp_v3(-hx, hy / 2, z), post), r = xf_box(vp_v3(hx, hy / 2, z), post);
        vp_xf top = xf_box(vp_v3(0, hy + 0.35f, z), vp_v3(hx + 0.35f, 0.35f, 0.35f));
        vp3d_scene_mesh(s, g_box, &l, VP3D_SOLID);
        vp3d_scene_mesh(s, g_box, &r, VP3D_SOLID);
        vp3d_scene_mesh(s, g_box, &top, VP3D_SOLID);
        /* the line on the floor: a row of checks */
        vp_color(d, 255, 255, 255);
        float y = 0.02f, sq = 0.75f;
        vp3d_scene_line(s, vp_v3(-hx, y, z - sq), vp_v3(hx, y, z - sq));
        vp3d_scene_line(s, vp_v3(-hx, y, z + sq), vp_v3(hx, y, z + sq));
        vp3d_scene_line(s, vp_v3(-hx, y, z), vp_v3(hx, y, z));
        for (float xx = -hx; xx <= hx + 0.01f; xx += sq)
            vp3d_scene_line(s, vp_v3(xx, y, z - sq), vp_v3(xx, y, z + sq));
        break;
    }
    case P_SEESAW:
    case P_BALLDROP:
    case P_TOPPLE:
        break;                                      /* physical: props_draw */
    }
}

/* ---- the beans ------------------------------------------------------------------- */

/* Its arms and legs, as their bodies are (props.c: physical, hinged at the
 * hips and shoulders): each one rounded shape with a fat end, drawn as its
 * outline. Beans farther off get stick limbs (the beam's time), the
 * farthest none. */
static void draw_limbs(const Bean *b, int fat)
{
    for (int k = 0; k < 4; k++) {
        vp_vec3 joint, tip;
        float r;
        if (!props_limb(b, k, &joint, &tip, &r)) continue;
        if (!fat) {
            vp3d_scene_line(g_scene, joint, tip);
            continue;
        }
        vp_vec3 up = vp_v3_normalize(vp_v3_sub(joint, tip));
        vp_vec3 any = fabsf(up.y) < 0.9f ? vp_v3(0, 1, 0) : vp_v3(1, 0, 0);
        vp_vec3 u = vp_v3_normalize(vp_v3_cross(up, any)), w = vp_v3_cross(u, up);
        vp_xf place = xf_basis(joint, u, up, w);
        vp3d_scene_mesh(g_scene, k < 2 ? g_leg : g_arm, &place, VP3D_SOLID);
    }
}

/* Its shadow: a dim ellipse on whatever is straight below it, smaller and
 * fainter the higher it is (where a jump will land). Its body hides the
 * part behind it. None over the void. */
static void draw_shadow(vp_dvg *d, const Bean *b)
{
    float floor_y;
    if (!props_bean_shadow(b, &floor_y)) return;
    float h = b->pos.y - floor_y;
    if (h < -0.3f || h > 8.0f) return;
    float k = 1.0f - fmaxf(h, 0) / 8.0f;           /* 1 on the floor, 0 eight meters up */
    float r = 0.5f * (0.5f + 0.5f * k);
    vp_color(d, (uint8_t)(70 * k + 20), (uint8_t)(70 * k + 20), (uint8_t)(90 * k + 30));
    vp_vec3 prev = vp_v3(0, 0, 0);
    for (int i = 0; i <= 12; i++) {
        float a = 6.28318f * i / 12;
        vp_vec3 p = vp_v3(b->pos.x + r * cosf(a), floor_y + 0.02f, b->pos.z + r * 0.8f * sinf(a));
        if (i) vp3d_scene_line(g_scene, prev, p);
        prev = p;
    }
}

/* Its mood on its face: brows over the eyes, their inner ends coming down
 * as it gets angrier; furious, steam puffing up off its head. In its own
 * frame (x), on the curve of its body. */
static void draw_mood(vp_dvg *d, const Bean *b, const vp_xf *x, float anger, float t)
{
    (void)b;
    static const float brow[2][4] = { { -0.19f, 1.30f, -0.02f, 1.31f },    /* the big eye's: outer x y, inner x y */
                                      { 0.17f, 1.23f, 0.05f, 1.235f } };
    vp_color(d, 255, 255, 255);
    for (int e = 0; e < 2; e++) {
        float ox = brow[e][0], oy = brow[e][1] + 0.02f * anger, ix = brow[e][2], iy = brow[e][3] - 0.08f * anger;
        vp_vec3 p[2];
        float px[2] = { ox, ix }, py[2] = { oy, iy };
        for (int k = 0; k < 2; k++) {
            float r = 0.44f + (py[k] - 1.07f) / 0.23f * (0.35f - 0.44f);
            p[k] = vp_xf_point(x, vp_v3(px[k], py[k], sqrtf(fmaxf(r * r - px[k] * px[k], 0.0f)) + 0.015f));
        }
        vp3d_scene_line(g_scene, p[0], p[1]);
    }
    if (anger < 0.8f) return;
    vp_color(d, 200, 200, 210);                     /* steam: puffs rising, growing, off the top */
    for (int k = 0; k < 2; k++) {
        float u = fmodf(t * 1.6f + k * 0.5f, 1.0f);
        float side = k ? 0.16f : -0.16f, rad = 0.04f + 0.07f * u;
        vp_vec3 c = vp_xf_point(x, vp_v3(side * (1 + u), BEAN_H - 0.05f + u * 0.4f, 0));
        vp_vec3 prev = vp_v3(0, 0, 0);
        for (int i = 0; i <= 6; i++) {
            float a = 6.28318f * i / 6;
            vp_vec3 q = vp_v3_add(c, vp_v3(cosf(a) * rad, sinf(a) * rad, 0));
            if (i) vp3d_scene_line(g_scene, prev, q);
            prev = q;
        }
    }
}

static void draw_bean(vp_dvg *d, const Bean *b, float t, float dist)
{
    if (b->state == BEAN_OUT || b->fallen > 0) return;
    float h = b->heading;
    vp_vec3 f = vp_v3(sinf(h), 0, cosf(h)), r = vp_v3(cosf(h), 0, -sinf(h)), up = vp_v3(0, 1, 0);
    vp_vec3 feet = b->pos;
    if (b->tumbling && b->body) {
        /* knocked over: as its body turns, tumbling about its middle */
        vp_xf p = vp_body_place(b->body);
        r = vp_v3(p.m[0], p.m[3], p.m[6]);
        up = vp_v3(p.m[1], p.m[4], p.m[7]);
        f = vp_v3(p.m[2], p.m[5], p.m[8]);
        feet = vp_v3_sub(vp_v3(p.t[0], p.t[1], p.t[2]), vp_v3_scale(up, BEAN_H / 2));
    }
    /* its mood: 0 calm .. 1 furious (trembling) */
    float anger = b->revenge > 0 ? 1.0f : fminf(1.0f, fmaxf(0.0f, (b->temper - 0.3f) / 0.7f));
    if (anger > 0.8f && !b->tumbling) feet = vp_v3_add(feet, vp_v3_scale(r, 0.03f * sinf(t * 55.0f)));
    /* lying down after a dive; wobbling when knocked over */
    float tilt = 0, roll = 0;
    if (b->stun > 0 && !b->tumbling) { tilt = 0.6f * sinf(b->stun * 9.0f); roll = 0.5f * cosf(b->stun * 7.0f); }
    if (tilt != 0) {
        vp_vec3 nu = vp_v3_add(vp_v3_scale(up, cosf(tilt)), vp_v3_scale(f, sinf(tilt)));
        f = vp_v3_sub(vp_v3_scale(f, cosf(tilt)), vp_v3_scale(up, sinf(tilt)));
        up = nu;
    }
    if (roll != 0) {
        vp_vec3 nu = vp_v3_add(vp_v3_scale(up, cosf(roll)), vp_v3_scale(r, sinf(roll)));
        r = vp_v3_sub(vp_v3_scale(r, cosf(roll)), vp_v3_scale(up, sinf(roll)));
        up = nu;
    }
    /* a running bounce, and the jump's stretch */
    float speed = sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
    float bob = (b->grounded && speed > 1) ? 0.06f * fabsf(sinf(t * 14.0f)) : 0;
    float sy = 1.0f + 0.22f * b->squash + bob, sxz = 1.0f - 0.1f * b->squash;
    /* standing still, it breathes: in, a little taller and rounder; each
     * bean at its own pace */
    if (b->grounded && b->stun <= 0 && !b->tumbling && speed < 1.5f) {
        float rest = 1.0f - speed / 1.5f;
        float breath = 0.5f + 0.5f * sinf(t * (2.2f + 0.15f * (b->id % 4)) + b->id * 1.7f);
        breath *= breath;                               /* a quick in, a slow out */
        sy  += rest * 0.05f * breath;
        sxz += rest * 0.04f * breath;
    }
    vp_xf x = xf_basis(feet, vp_v3_scale(r, sxz), vp_v3_scale(up, sy), vp_v3_scale(f, sxz));
    vp_color(d, b->r, b->g, b->b);
    vp3d_scene_mesh(g_scene, g_bean, &x, VP3D_SOLID);
    if (g_eyes && dist < 40.0f) {
        vp_color(d, 255, 255, 255);
        vp3d_scene_mesh(g_scene, g_eyes, &x, VP3D_SOLID);
        if (anger > 0.08f) draw_mood(d, b, &x, anger, t);
        vp_color(d, b->r, b->g, b->b);              /* its arms and legs: its own color again */
    }
    if (dist < 32.0f) draw_limbs(b, dist < 20.0f);
    if (dist < 25.0f) draw_shadow(d, b);
}

static void draw_crown(vp_dvg *d, vp_vec3 p, float t)
{
    vp_color(d, 255, 220, 0);
    float y0 = p.y + BEAN_H + 0.25f + 0.08f * sinf(t * 3), r = 0.32f;
    vp_vec3 prev = vp_v3(0, 0, 0);
    for (int i = 0; i <= 10; i++) {
        float a = 6.28318f * i / 10 + t;
        float y = y0 + ((i & 1) ? 0.12f : 0.38f);
        vp_vec3 q = vp_v3(p.x + r * sinf(a), y, p.z + r * cosf(a));
        if (i) vp3d_scene_line(g_scene, prev, q);
        prev = q;
    }
}

void render_world(vp_dvg *d, const vp3d_view *view, const Course *c, float t,
                  const Bean *beans, int n, int marked, int crowned, RenderHide hide)
{
    vp_vec3 eye = vp3d_eye(view);
    float near = eye.z - 8, far = eye.z + view->far_z;
    vp3d_scene_begin(g_scene, d, view);
    for (int i = 0; i < c->nparts; i++) {
        float z0, z1;
        part_extent(&c->parts[i], &z0, &z1);
        if (z1 < near || z0 > far) continue;
        draw_part(d, &c->parts[i], c->parts, t);
    }
    for (int i = 0; i < n; i++) {
        if (beans[i].pos.z < near || beans[i].pos.z > far) continue;
        draw_bean(d, &beans[i], t, vp_v3_length(vp_v3_sub(beans[i].pos, eye)));
    }
    props_draw(d, g_scene, g_box, g_plank, g_ball, near, far);
    if (crowned >= 0 && crowned < n) draw_crown(d, beans[crowned].pos, t);
    if (hide) hide(g_scene);
    vp3d_scene_end(g_scene);

    /* the player's bean: an arrow over its head, never hidden */
    if (marked >= 0 && marked < n && beans[marked].state != BEAN_OUT && beans[marked].fallen <= 0) {
        float sx, sy;
        vp_vec3 p = beans[marked].pos;
        if (vp3d_project(view, vp_v3(p.x, p.y + BEAN_H + 0.6f, p.z), &sx, &sy)) {
            float s = 55;
            vp_color(d, 255, 255, 0);
            vp_moveto(d, (int)(sx - s), (int)(sy + s * 1.6f));
            vp_lineto(d, (int)(sx + s), (int)(sy + s * 1.6f));
            vp_lineto(d, (int)sx, (int)sy);
            vp_lineto(d, (int)(sx - s), (int)(sy + s * 1.6f));
        }
    }
}

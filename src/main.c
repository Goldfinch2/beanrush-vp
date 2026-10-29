/* Bean Rush — a show of obstacle races on the VectorPie SDK.
 *
 * You and nine computer beans run three courses. Round 1: the first six
 * across the line go through. Round 2: three of the six. The final: the first
 * one home wins the crown. Fall off and you come back at the last
 * checkpoint; get knocked over and you lose time.
 *
 * Controls (the cabinet's own, from advmame.rc, until changed on the
 * CONTROLS page: TAB, or START 1 and START 2 together, on the title):
 *   the stick (analog or digital) runs, button 1 jumps, button 2 dives,
 *   button 3 punches the nearest bean (a shove instead, if it is at an
 *   edge). Beans bumped too often get furious and get even.
 *   START 1 plays (a credit), the exit button goes back to the menu.
 *
 * Scoring: in each round you go through, points for your place and for
 * time to spare; the crown is worth 3000. The best ten are kept (initials).
 */
#include <time.h>
#include "beanrush.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W           VP_DVG_COORD_MAX
#define RACE_LIMIT  180.0f          /* s: a race that runs longer is over */
#define AFTER_QUALIFY 2.5f          /* s of the race still shown once you are through */
#define ENTRY_LIMIT 30.0f

#define KEY_TAB        43           /* SDL scancodes */
#define KEY_DELETE     76
#define KEY_KP_PERIOD  99

enum { ST_TITLE, ST_CONTROLS, ST_INTRO, ST_COUNTDOWN, ST_RACE, ST_RESULT, ST_SHOW_OVER, ST_INITIALS };

/* The round's field and how many go through. */
static const int FIELD[3]   = { 10, 6, 3 };
static const int QUALIFY[3] = { 6, 3, 1 };

/* The computer's beans: anything but the player's pink. */
static const uint8_t COLORS[][3] = {
    { 0, 255, 255 }, { 255, 255, 0 }, { 0, 255, 0 }, { 255, 128, 0 }, { 180, 100, 255 },
    { 255, 255, 255 }, { 80, 160, 255 }, { 255, 80, 80 }, { 160, 255, 160 }, { 255, 200, 120 },
    { 120, 255, 255 }, { 255, 255, 150 }, { 200, 200, 80 }, { 100, 255, 200 }, { 150, 200, 255 },
};
#define PLAYER_R 255
#define PLAYER_G 60
#define PLAYER_B 200

/* ---- the game's functions (its CONTROLS page) ------------------------------------ */

enum { FUNCTIONS = 7 };
static const char *const FUNCTION_NAME[FUNCTIONS] = { "up", "down", "left", "right", "jump", "dive", "punch" };
static const char *const FUNCTION_LABEL[FUNCTIONS] = { "RUN AHEAD", "RUN BACK", "LEFT", "RIGHT", "JUMP", "DIVE", "PUNCH" };
static const char *const FUNCTION_STANDARD[FUNCTIONS] = { "p1_up", "p1_down", "p1_left", "p1_right",
                                                          "p1_button1", "p1_button2", "p1_button3" };

static struct {
    int        state;
    float      state_time;
    double     time;            /* the game's clock */
    int        demo;            /* the title's race: nobody plays */
    int        count_shown;     /* the countdown's last number beeped */
    /* development (environment): BEANRUSH_AUTOPLAY=1 plays shows by itself,
     * BEANRUSH_SHOTS=<folder> saves a picture every half second (or every
     * BEANRUSH_SHOT_EVERY seconds, from BEANRUSH_SHOT_FROM on: a clip),
     * BEANRUSH_ROUND=2 or 3 starts the show at that round,
     * BEANRUSH_COURSE=1..7 makes that course the first round's,
     * BEANRUSH_FACECAM=1 films the player from in front */
    int        facecam;
    int        autoplay;
    const char *shots;
    double     next_shot, shot_every;
    vp_input  *in;
    vp_audio  *audio;
    vp_credits credits;

    Course     course;
    int        round;           /* 0..2 */
    int        show[3];         /* the show's courses, a round each */
    float      clock;           /* the course's clock: the parts move by it */
    float      race_time;       /* since GO */
    Bean       beans[MAX_BEANS];
    int        nbeans;
    int        player;          /* its index, -1 in the demo */
    int        finished;        /* how many are through */
    float      player_through;  /* s since the player finished, 0 if not */
    int        crowned;         /* the winner's index in the final, -1 */
    int        qualified;       /* the player went through this round */
    unsigned   rng;

    int        score;
    int        round_points;
    int        best_round;      /* the furthest round reached, for the summary */
    int        won;

    vp3d_view  view;
    vp_vec3    cam_eye, cam_at;
    int        cam_set;

    vp_score   table[VP_HISCORE_MAX];
    int        table_count;
    int        last_rank;
    vp_initials entry;

    vp_capture capture;
    int        ctl_row;
    char       ctl_said[96];

    /* sounds */
    vp_sound  *snd_jump, *snd_dive, *snd_hit, *snd_fall, *snd_beep, *snd_go;
    vp_sound  *snd_through, *snd_out, *snd_crown, *snd_coin, *snd_menu, *snd_boing, *snd_punch;
    vp_voice   music;
    int        track;           /* the one playing */
} G;

static void set_state(int s)
{
    G.state = s;
    G.state_time = 0;
}

static void play(vp_sound *s, float vol, float pitch)
{
    if (s) vp_sound_play(G.audio, s, vol, pitch, 0);
}

/* ---- sounds made in code -------------------------------------------------------- */

#define RATE 22050
#define TAU  6.2831853f

static vp_sound *synth(float seconds, float (*gen)(float t, int i))
{
    int n = (int)(seconds * RATE);
    float *buf = malloc(sizeof(float) * (size_t)n);
    vp_sound *s = NULL;
    if (!buf || !G.audio) { free(buf); return NULL; }
    for (int i = 0; i < n; i++) buf[i] = gen((float)i / RATE, i);
    vp_sound_from_samples(G.audio, buf, n, 1, RATE, &s);
    free(buf);
    return s;
}

static float noise(int i)
{
    unsigned x = (unsigned)i * 2654435761u;
    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
    return (float)(x & 0xffff) / 32768.0f - 1.0f;
}

static float sq(float ph) { return sinf(ph) > 0 ? 1.0f : -1.0f; }

static float env(float t, float len) { float k = 1.0f - t / len; return k < 0 ? 0 : k * k; }

static float g_jump(float t, int i) { (void)i; return 0.35f * sq(TAU * (260 + 2600 * t) * t) * env(t, 0.18f); }
static float g_dive(float t, int i) { return 0.4f * noise(i) * env(t, 0.28f) * (0.3f + 0.7f * sinf(TAU * 3 * t)); }
static float g_hit(float t, int i)  { return (0.7f * sinf(TAU * 85 * t) + 0.4f * noise(i) * env(t, 0.06f)) * env(t, 0.35f); }
static float g_fall(float t, int i) { (void)i; return 0.35f * sinf(TAU * (900 - 700 * t) * t) * env(t, 0.75f); }
static float g_beep(float t, int i) { (void)i; return 0.35f * sq(TAU * 660 * t) * env(t, 0.14f); }
static float g_go(float t, int i)   { (void)i; return 0.3f * (sq(TAU * 990 * t) + 0.5f * sinf(TAU * 1980 * t)) * env(t, 0.5f); }
/* a spring: a rising wobble */
static float g_boing(float t, int i) { (void)i; return 0.35f * sinf(TAU * (180 + 500 * t + 60 * sinf(TAU * 18 * t)) * t) * env(t, 0.3f); }
/* a big cartoon punch: a sharp slap, a heavy thump dropping low, a burst of
 * noise, a short tail */
static float g_punch(float t, int i)
{
    float slap = noise(i) * env(t, 0.025f) * 0.9f;
    float f = 45 + 135 * expf(-t * 18);                      /* 180 Hz falling to 45 */
    float thump = sinf(TAU * f * t) * env(t, 0.32f) * 1.1f;
    float crunch = noise(i / 3) * env(t, 0.12f) * 0.5f;
    float v = slap + thump + crunch;
    return v > 1 ? 1 : v < -1 ? -1 : v;
}
static float g_coin(float t, int i) { (void)i; return 0.3f * sq(TAU * (t < 0.06f ? 990 : 1320) * t) * env(t, 0.2f); }
static float g_menu(float t, int i) { (void)i; return 0.25f * sinf(TAU * 1200 * t) * env(t, 0.05f); }

static float notes(float t, const float *freq, int n, float step, float vol)
{
    int k = (int)(t / step);
    if (k >= n) k = n - 1;
    float tt = t - k * step;
    return vol * sq(TAU * freq[k] * tt) * env(tt, step * 1.3f);
}
static float g_through(float t, int i)
{
    static const float f[] = { 523, 659, 784, 1047, 1047 };
    (void)i; return notes(t, f, 5, 0.11f, 0.3f);
}
static float g_out(float t, int i)
{
    static const float f[] = { 392, 330, 262, 196 };
    (void)i; return notes(t, f, 4, 0.2f, 0.3f);
}
static float g_crown(float t, int i)
{
    static const float f[] = { 523, 523, 523, 659, 784, 659, 784, 1047, 1047 };
    (void)i; return notes(t, f, 9, 0.13f, 0.28f);
}

static void load_sounds(void)
{
    G.snd_jump = synth(0.18f, g_jump);
    G.snd_dive = synth(0.28f, g_dive);
    G.snd_hit = synth(0.35f, g_hit);
    G.snd_fall = synth(0.75f, g_fall);
    G.snd_beep = synth(0.14f, g_beep);
    G.snd_go = synth(0.5f, g_go);
    G.snd_through = synth(0.7f, g_through);
    G.snd_out = synth(0.9f, g_out);
    G.snd_crown = synth(1.3f, g_crown);
    G.snd_coin = synth(0.2f, g_coin);
    G.snd_boing = synth(0.3f, g_boing);
    G.snd_punch = synth(0.45f, g_punch);
    G.snd_menu = synth(0.05f, g_menu);
}

/* The music (in data/music.dat, src/music.c; credited in data/CREDITS.txt),
 * streamed and looped: "Funny Toys" (without its drum fills) on the title
 * and between races, and each race one of the others, picked at random
 * (never the last one again). A change of track crossfades. The names are
 * the tracks' in music/, without .ogg (CMakeLists.txt packs those). */
enum { TRACK_TITLE, TRACK_RACES };
static const char *const TRACK_FILE[] = {
    "funny_toys_loop_no_drumfills",
    "bunny_is_so_funny_long_loop",
    "funny_toys_loop",
    "shine_upon_us",
    "bunnies_are_sleeping_loop",
};
#define NTRACKS ((int)(sizeof TRACK_FILE / sizeof TRACK_FILE[0]))

static int race_track(void)
{
    static int seeded, last = -1;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
    int k;
    do k = TRACK_RACES + rand() % (NTRACKS - TRACK_RACES); while (k == last);
    return last = k;
}

static void music(int track, float vol)
{
    if (!G.audio) return;
    if (G.music && G.track == track) {
        vp_voice_set_volume(G.audio, G.music, vol, 0.6f);
        return;
    }
    if (G.music) vp_voice_stop(G.audio, G.music, 0.8f);
    size_t size;
    unsigned char *ogg = music_track(TRACK_FILE[track], &size);
    G.music = ogg ? vp_music_play_mem(G.audio, ogg, size, vol, 1, 0.8f) : 0;
    free(ogg);
    G.track = track;
}

/* ---- a race ------------------------------------------------------------------------ */

/* Put the beans still in the show on the start line of `course` (round
 * `round` of the show). */
static void setup_race(int round, int course, int demo)
{
    G.round = round;
    course_build(&G.course, course, (unsigned)(rnd(&G.rng) * 16777216.0f));
    G.clock = 0;
    G.race_time = 0;
    G.finished = 0;
    G.player_through = 0;
    G.crowned = -1;
    G.qualified = 0;
    int slot = 0, field = 0;
    for (int i = 0; i < G.nbeans; i++) if (G.beans[i].state != BEAN_OUT) field++;
    /* the player starts somewhere in the pack, not always in front */
    int pslot = demo ? -1 : (int)(rnd(&G.rng) * field);
    for (int i = 0; i < G.nbeans; i++) {
        Bean *b = &G.beans[i];
        if (b->state == BEAN_OUT) continue;
        int s;
        if (b->human) s = pslot;
        else {
            if (slot == pslot) slot++;              /* the player's place */
            s = slot++;
        }
        if (s >= field) s = field - 1;
        bean_place(b, &G.course, s, field);
        b->lane = b->human ? 0 : b->pos.x / 6.0f;
    }
    props_build(&G.course, G.beans, G.nbeans, G.rng);
    G.cam_set = 0;
}

/* A new show: ten beans, the player first. */
static void new_field(int demo)
{
    memset(G.beans, 0, sizeof G.beans);
    G.nbeans = MAX_BEANS;
    for (int i = 0; i < G.nbeans; i++) {
        Bean *b = &G.beans[i];
        b->state = BEAN_RACING;
        b->human = !demo && i == 0;
        if (b->human) {
            b->r = PLAYER_R; b->g = PLAYER_G; b->b = PLAYER_B;
        } else {
            const uint8_t *c = COLORS[(i + (demo ? 3 : 0)) % (int)(sizeof COLORS / sizeof COLORS[0])];
            b->r = c[0]; b->g = c[1]; b->b = c[2];
        }
        /* the computer's beans: from steady to quick, and some careless */
        b->skill = 0.80f + 0.17f * rnd(&G.rng);
    }
    G.player = demo ? -1 : 0;
}

static void start_demo(void)
{
    G.demo = 1;
    new_field(1);
    setup_race(0, (int)(rnd(&G.rng) * course_count()) % course_count(), 1);
    G.race_time = 0;
}

static void start_show(void)
{
    G.demo = 0;
    G.score = 0;
    G.won = 0;
    G.best_round = 0;
    new_field(0);
    int first = getenv("BEANRUSH_ROUND") ? atoi(getenv("BEANRUSH_ROUND")) - 1 : 0;
    if (first < 0 || first > 2) first = 0;
    if (first > 0)                                  /* the field that round starts with */
        for (int i = FIELD[first]; i < G.nbeans; i++) G.beans[i].state = BEAN_OUT;
    /* the show's courses: for each round, one of those made for it */
    for (int r = 0; r < 3; r++) {
        int pick[16], n = 0;
        for (int k = 0; k < course_count() && n < 16; k++)
            if (course_round(k) == r) pick[n++] = k;
        G.show[r] = n ? pick[(int)(rnd(&G.rng) * n) % n] : 0;
    }
    if (getenv("BEANRUSH_COURSE")) {
        int k = atoi(getenv("BEANRUSH_COURSE")) - 1;
        if (k >= 0 && k < course_count()) G.show[first] = k;
    }
    setup_race(first, G.show[first], 0);
    play(G.snd_go, 0.6f, 0.8f);
    set_state(ST_INTRO);
}

/* The places of those still running, by how far they got. */
static void fill_places(int qualify)
{
    while (G.finished < qualify) {
        int best = -1;
        for (int i = 0; i < G.nbeans; i++) {
            Bean *b = &G.beans[i];
            if (b->state != BEAN_RACING) continue;
            if (best < 0 || bean_progress(b) > bean_progress(&G.beans[best])) best = i;
        }
        if (best < 0) break;
        G.beans[best].state = BEAN_FINISHED;
        G.beans[best].place = ++G.finished;
        G.beans[best].finish_time = RACE_LIMIT;
    }
    for (int i = 0; i < G.nbeans; i++)
        if (G.beans[i].state == BEAN_RACING) G.beans[i].state = BEAN_OUT;
}

/* One step of the race for everyone. `live`: the clock of the race runs
 * (else the beans wait on the line while the course moves). */
static void bean_sounds(int i, int ev)
{
    const Bean *b = &G.beans[i];
    if (i == G.player) {
        if (ev & EV_JUMP) play(G.snd_jump, 0.6f, 1.0f);
        if (ev & EV_DIVE) play(G.snd_dive, 0.6f, 1.0f);
        if (ev & EV_HIT)  play(G.snd_hit, 0.9f, 1.0f);
        if (ev & EV_FALL) play(G.snd_fall, 0.7f, 1.0f);
        if (ev & EV_PUNCH) play(G.snd_punch, 1.0f, 1.0f);
        if (ev & EV_SHOVE) play(G.snd_boing, 0.9f, 0.7f);
        if (ev & EV_WHIFF) play(G.snd_dive, 0.4f, 1.6f);
    } else if ((ev & (EV_PUNCH | EV_SHOVE)) && G.player >= 0 && fabsf(b->pos.z - G.beans[G.player].pos.z) < 14) {
        play((ev & EV_PUNCH) ? G.snd_punch : G.snd_boing, 0.55f, 1.0f);   /* a fight nearby */
    } else if ((ev & EV_HIT) && G.player >= 0 && fabsf(b->pos.z - G.beans[G.player].pos.z) < 12) {
        play(G.snd_hit, 0.3f, 1.3f);                /* someone near you got bonked */
    }
}

/* One step of the race: the beans' controls steer their bodies, the physics
 * moves everything, the beans see where they went. `live`: the race has
 * started (else the beans wait on the line while the course moves). */
static void race_step(float dt, int live)
{
    int ev[MAX_BEANS] = { 0 };
    G.clock += dt;
    if (live) G.race_time += dt;
    for (int i = 0; i < G.nbeans; i++) {
        Bean *b = &G.beans[i];
        if (b->state == BEAN_OUT) continue;
        BeanInput in;
        memset(&in, 0, sizeof in);
        if (!live) {
            /* waiting at the start: no controls */
        } else if (b->human && !G.autoplay) {
            in.x = vp_input_axis(G.in, "p1_stickx");
            in.z = -vp_input_axis(G.in, "p1_sticky");
            if (vp_input_held(G.in, "left"))  in.x = -1;
            if (vp_input_held(G.in, "right")) in.x = 1;
            if (vp_input_held(G.in, "up"))    in.z = 1;
            if (vp_input_held(G.in, "down"))  in.z = -1;
            in.jump = vp_input_pressed(G.in, "jump");
            in.dive = vp_input_pressed(G.in, "dive");
            in.punch = vp_input_pressed(G.in, "punch");
        } else if (b->revenge > 0) {
            bean_revenge(b, G.beans, &in);           /* furious: getting even */
        } else {
            bean_ai(b, &G.course, G.clock, &in, &G.rng);
        }
        bean_mood(b, G.beans, G.nbeans, dt);
        bean_control(b, &in, &G.course, G.clock, dt, &ev[i]);
        if (in.punch && live) ev[i] |= bean_strike(b, G.beans, G.nbeans, &G.course, G.clock);
    }

    unsigned bounced;
    unsigned knocked = props_step(&G.course, G.beans, G.nbeans, G.clock - dt, dt, &bounced);
    if (G.player >= 0 && (bounced >> G.player & 1)) play(G.snd_boing, 0.6f, 0.9f + rnd(&G.rng) * 0.3f);

    int qualify = G.demo ? MAX_BEANS : QUALIFY[G.round];
    for (int i = 0; i < G.nbeans; i++) {
        Bean *b = &G.beans[i];
        if (b->state == BEAN_OUT) continue;
        int racing = b->state == BEAN_RACING, more = 0;
        bean_sync(b, &G.course, G.clock, dt, &more);
        ev[i] |= more | ((knocked >> i & 1) ? EV_HIT : 0);
        bean_sounds(i, ev[i]);
        if (live && racing && b->state == BEAN_FINISHED) {
            if (G.finished < qualify) {
                b->place = ++G.finished;
                b->finish_time = G.race_time;
            } else {
                b->state = BEAN_RACING;             /* the spots are gone: it runs on, out */
            }
        }
    }
}

/* ---- the camera --------------------------------------------------------------------- */

static int leader(void)
{
    int best = -1;
    for (int i = 0; i < G.nbeans; i++) {
        if (G.beans[i].state == BEAN_OUT) continue;
        if (G.beans[i].state == BEAN_FINISHED && G.state == ST_TITLE) continue;
        if (best < 0 || bean_progress(&G.beans[i]) > bean_progress(&G.beans[best])) best = i;
    }
    return best < 0 ? 0 : best;
}

static void camera_follow(int who, float dt)
{
    const Bean *b = &G.beans[who];
    vp_vec3 p = b->pos;
    if (b->fallen > 0 || p.y < 0) p.y = 0;          /* falling: stay up, watch it go */
    vp_vec3 eye = vp_v3(p.x * 0.6f, p.y + 4.6f, p.z - 8.5f);
    vp_vec3 at = vp_v3(p.x * 0.8f, p.y + 1.0f, p.z + 6.0f);
    if (G.facecam) {                                /* development: in front, looking at it */
        eye = vp_v3(p.x, p.y + 1.6f, p.z + 3.2f);
        at = vp_v3(p.x, p.y + 1.0f, p.z);
    }
    if (!G.cam_set) { G.cam_eye = eye; G.cam_at = at; G.cam_set = 1; }
    float k = 1.0f - expf(-dt * 5.0f);
    G.cam_eye = vp_v3_add(G.cam_eye, vp_v3_scale(vp_v3_sub(eye, G.cam_eye), k));
    G.cam_at = vp_v3_add(G.cam_at, vp_v3_scale(vp_v3_sub(at, G.cam_at), k));
}

/* The round's introduction: a flight from the finish back to the start. */
static void camera_flyover(float s)
{
    float len = G.course.length;
    float u = s < 0 ? 0 : s > 1 ? 1 : s;
    u = u * u * (3 - 2 * u);
    float z = len - 4 - (len - 2) * u;
    G.cam_eye = vp_v3(9.0f * (1 - u), 9.0f - 4.4f * u, z - 9.0f);
    G.cam_at = vp_v3(0, 0.5f, z + 6.0f);
    G.cam_set = 1;
}

/* ---- the CONTROLS page ------------------------------------------------------------------ */

static void define_controls(vp_input *in)
{
    for (int i = 0; i < FUNCTIONS; i++) vp_input_define(in, FUNCTION_NAME[i], FUNCTION_STANDARD[i]);
    vp_input_reserve_key(in, KEY_TAB);
    vp_input_load(in, NULL);
}

static void save_controls(void)
{
    if (vp_input_save(G.in, NULL) != VP_OK)
        fprintf(stderr, "beanrush: controls not saved: %s\n", vp_last_error());
}

static void controls_page(void)
{
    vp_input *in = G.in;
    if (G.capture.capturing) {
        if (vp_capture_step(&G.capture, in) == VP_CAPTURE_CHANGED) {
            save_controls();
            play(G.snd_menu, 0.8f, 1.0f);
        }
    } else if (vp_input_key_pressed(in, KEY_TAB)) {
        play(G.snd_menu, 0.8f, 1.0f);
        set_state(ST_TITLE);
        return;
    } else {
        if (vp_input_menu_up(in) || vp_input_menu_down(in)) {
            G.ctl_row = (G.ctl_row + (vp_input_menu_up(in) ? FUNCTIONS : 1)) % (FUNCTIONS + 1);
            vp_capture_moved(&G.capture);
            play(G.snd_menu, 0.6f, 1.0f);
        }
        int left = vp_input_menu_left_pressed(in), right = vp_input_menu_right_pressed(in);
        vp_capture_filter(&G.capture, &left, &right);
        if (G.ctl_row < FUNCTIONS) {
            const char *name = FUNCTION_NAME[G.ctl_row];
            if (vp_input_menu_confirm(in)) {
                vp_capture_begin(&G.capture, in, name);
            } else if (left || right) {
                vp_capture_toggle(&G.capture, in, name);
                save_controls();
            } else if (vp_input_key_pressed(in, KEY_DELETE) || vp_input_key_pressed(in, KEY_KP_PERIOD)) {
                vp_capture_standard(&G.capture, in, name);
                save_controls();
            }
        } else if (vp_input_menu_confirm(in)) {
            play(G.snd_menu, 0.8f, 1.0f);
            set_state(ST_TITLE);
            return;
        }
    }
    const char *said = vp_capture_said(&G.capture);
    snprintf(G.ctl_said, sizeof G.ctl_said, "%s", said ? said : "");
}

/* ---- the show's steps ------------------------------------------------------------------ */

static void end_race(void)
{
    int qualify = QUALIFY[G.round];
    fill_places(qualify);
    Bean *p = &G.beans[G.player];
    G.qualified = p->state == BEAN_FINISHED;
    G.round_points = 0;
    if (G.qualified) {
        int field = FIELD[G.round];
        G.round_points = 100 * (field - p->place + 1) * (G.round + 1);
        float spare = RACE_LIMIT - p->finish_time;
        if (spare > 0) G.round_points += (int)(spare * 5);
        if (G.round == 2) { G.round_points += 3000; G.won = 1; G.crowned = G.player; }
        G.score += G.round_points;
        play(G.round == 2 ? G.snd_crown : G.snd_through, 0.8f, 1.0f);
    } else {
        if (G.round == 2) {
            for (int i = 0; i < G.nbeans; i++)
                if (G.beans[i].state == BEAN_FINISHED && G.beans[i].place == 1) G.crowned = i;
        }
        play(G.snd_out, 0.8f, 1.0f);
    }
    G.best_round = G.round;
    if (G.autoplay) {
        printf("beanrush: round %d %s: %s, place %d, %.1f s, +%d, score %d\n", G.round + 1, G.course.name,
               G.qualified ? "through" : "out", p->place, p->finish_time, G.round_points, G.score);
        printf("  finish at z %.0f; furthest:", G.course.finish_z);
        for (int i = 0; i < G.nbeans; i++)
            if (G.beans[i].state != BEAN_OUT || G.beans[i].best_z > 0) printf(" %.0f%s", G.beans[i].best_z, i == G.player ? "*" : "");
        printf("\n  checkpoints at z:");
        for (int k = 0; k < G.course.nchecks; k++) printf(" %.0f", G.course.checks[k].z);
        printf("\n");
        fflush(stdout);
    }
    set_state(ST_RESULT);
}

static void after_result(void)
{
    if (G.qualified && G.round < 2) {
        setup_race(G.round + 1, G.show[G.round + 1], 0);
        set_state(ST_INTRO);
        return;
    }
    set_state(ST_SHOW_OVER);
}

static void after_show(void)
{
    G.last_rank = -1;
    if (vp_hiscore_rank(G.score) >= 0) {
        vp_initials_begin(&G.entry, 1, ENTRY_LIMIT);
        set_state(ST_INITIALS);
        return;
    }
    start_demo();
    set_state(ST_TITLE);
}

/* ---- update -------------------------------------------------------------------------------- */

static void update(vp_game *g, float dt)
{
    vp_input *in = vp_game_input(g);
    G.in = in;
    G.state_time += dt;
    G.time = vp_game_time(g);
    if (G.shots && G.time >= G.next_shot) {        /* between frames: the last one sent */
        char path[1024];
        snprintf(path, sizeof path, "%s/shot_%07.1f.png", G.shots, G.time);
        G.next_shot = G.time + G.shot_every;
        vp_dvg_save_png(vp_game_output(g), path, 640, 480);
    }

    if (vp_credits_update(&G.credits, in)) play(G.snd_coin, 0.8f, 1.0f);
    if (G.state == ST_TITLE) vp_credits_blink(&G.credits, vp_game_time(g));
    else vp_lamps_set(0);

    switch (G.state) {
    case ST_TITLE:
        music(TRACK_TITLE, 0.6f);
        if (vp_input_key_pressed(in, KEY_TAB) ||
            (vp_input_held(in, "start1") && vp_input_held(in, "start2") &&
             (vp_input_pressed(in, "start1") || vp_input_pressed(in, "start2")))) {
            memset(&G.capture, 0, sizeof G.capture);
            G.ctl_row = 0;
            G.ctl_said[0] = 0;
            play(G.snd_menu, 0.8f, 1.0f);
            set_state(ST_CONTROLS);
            break;
        }
        if ((vp_input_pressed(in, "start1") && vp_credits_take(&G.credits, 1)) ||
            (G.autoplay && G.state_time > 3.0f)) {
            start_show();
            break;
        }
        race_step(dt, 1);
        camera_follow(leader(), dt);
        if (G.finished >= 3 || G.race_time > 70) start_demo();
        break;

    case ST_CONTROLS:
        race_step(dt, 1);
        camera_follow(leader(), dt);
        if (G.finished >= 3 || G.race_time > 70) start_demo();
        controls_page();
        break;

    case ST_INTRO:
        music(TRACK_TITLE, 0.45f);
        race_step(dt, 0);
        camera_flyover(G.state_time / 4.5f);
        if (G.state_time > 5.0f || (G.state_time > 1.0f && vp_input_pressed(in, "jump"))) {
            G.cam_set = 0;
            G.count_shown = -1;
            set_state(ST_COUNTDOWN);
        }
        break;

    case ST_COUNTDOWN: {
        int now = (int)G.state_time;
        if (now != G.count_shown && now < 3) {
            play(G.snd_beep, 0.7f, 1.0f);
            G.count_shown = now;
        }
        race_step(dt, 0);
        camera_follow(G.player, dt);
        if (G.state_time >= 3.0f) {
            play(G.snd_go, 0.8f, 1.0f);
            music(race_track(), 0.55f);
            set_state(ST_RACE);
        }
        break;
    }

    case ST_RACE: {
        race_step(dt, 1);
        camera_follow(G.player, dt);
        Bean *p = &G.beans[G.player];
        if (p->state == BEAN_FINISHED) {
            if (G.player_through == 0) {
                play(G.snd_through, 0.7f, 1.0f);
                if (G.round == 2) G.crowned = G.player;
            }
            G.player_through += dt;
        }
        int qualify = QUALIFY[G.round];
        if (G.finished >= qualify || G.player_through > AFTER_QUALIFY || G.race_time > RACE_LIMIT)
            end_race();
        break;
    }

    case ST_RESULT:
        race_step(dt, 1);
        camera_follow(G.crowned >= 0 && G.round == 2 ? G.crowned : G.player, dt);
        if (G.state_time > 4.0f || (G.state_time > 1.5f && vp_input_pressed(in, "jump")))
            after_result();
        break;

    case ST_SHOW_OVER:
        music(TRACK_TITLE, 0.45f);
        race_step(dt, 1);
        camera_follow(G.crowned >= 0 ? G.crowned : G.player, dt);
        if (G.state_time > 5.0f || (G.state_time > 1.5f && vp_input_pressed(in, "jump")))
            after_show();
        break;

    case ST_INITIALS:
        race_step(dt, 1);
        camera_follow(G.player, dt);
        if (vp_initials_update(&G.entry, in, dt)) {
            int r = vp_hiscore_insert(G.entry.name, G.score);
            if (r < -1) fprintf(stderr, "beanrush: %s\n", vp_last_error());
            G.last_rank = r;
            G.table_count = vp_hiscore_load(G.table);
            start_demo();
            set_state(ST_TITLE);
            G.state_time = -7.0f;               /* the table first, the new name blinking */
        }
        break;
    }
}

/* ---- draw ---------------------------------------------------------------------------------- */

static void text(vp_dvg *d, const char *s, float x, float y, float h, int align)
{
    vp_text(d, s, x, y, h, align);
}

/* The big lettering: white with a pink border (red, gold...), bouncing. It
 * hides the world behind it: each one drawn is noted, and the world, drawn
 * after the lettering, puts its screens in the scene (hide_lettering). */
#define MAX_BIG 6
static struct { char s[40]; float y, h; } g_big[MAX_BIG];
static int g_nbig;

/* All the other text: the same comic letters, plain (no border, still). */
static void plain(void)
{
    vp_text_font(VP_FONT_COMIC);
    vp_text_border(0, 0, 0);
    vp_text_wobble(0, 0);
}

static void big(vp_dvg *d, const char *s, float y, float h, int r, int g, int b, int br, int bg, int bb)
{
    vp_text_font(VP_FONT_COMIC);
    vp_text_border((uint8_t)br, (uint8_t)bg, (uint8_t)bb);
    vp_text_wobble(1.0f, G.time);
    vp_color(d, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    vp_text(d, s, W / 2.0f, y, h, VP_TEXT_CENTER);
    plain();
    if (g_nbig < MAX_BIG) {
        snprintf(g_big[g_nbig].s, sizeof g_big[g_nbig].s, "%s", s);
        g_big[g_nbig].y = y;
        g_big[g_nbig].h = h;
        g_nbig++;
    }
}

static void hide_lettering(vp3d_scene *scene)
{
    vp_text_font(VP_FONT_COMIC);
    vp_text_wobble(1.0f, G.time);
    for (int i = 0; i < g_nbig; i++)
        vp3d_scene_text(scene, g_big[i].s, W / 2.0f, g_big[i].y, g_big[i].h, VP_TEXT_CENTER);
    plain();
}
#define PINK_BORDER   255, 40, 170
#define WHITE         255, 255, 255

static const char *ordinal(int n)
{
    static char buf[16];
    const char *suf = (n % 100 >= 11 && n % 100 <= 13) ? "TH" :
                      n % 10 == 1 ? "ST" : n % 10 == 2 ? "ND" : n % 10 == 3 ? "RD" : "TH";
    snprintf(buf, sizeof buf, "%d%s", n, suf);
    return buf;
}

static int player_position(void)
{
    const Bean *p = &G.beans[G.player];
    if (p->state == BEAN_FINISHED) return p->place;
    int pos = 1;
    float mine = bean_progress(p);
    for (int i = 0; i < G.nbeans; i++)
        if (i != G.player && G.beans[i].state != BEAN_OUT && bean_progress(&G.beans[i]) > mine) pos++;
    return pos;
}

static void draw_hud(vp_dvg *d)
{
    char line[64];
    plain();
    vp_color(d, 0, 200, 255);
    snprintf(line, sizeof line, "ROUND %d  %s", G.round + 1, G.course.name);
    text(d, line, 120, 3900, 85, VP_TEXT_LEFT);
    vp_color(d, 255, 255, 255);
    if (G.round == 2) snprintf(line, sizeof line, "FINAL");
    else snprintf(line, sizeof line, "THROUGH %d/%d", G.finished, QUALIFY[G.round]);
    text(d, line, W - 120.0f, 3900, 85, VP_TEXT_RIGHT);
    snprintf(line, sizeof line, "%s", ordinal(player_position()));
    vp_color(d, 255, 255, 0);
    text(d, line, 120, 3730, 120, VP_TEXT_LEFT);
    int left = (int)(RACE_LIMIT - G.race_time + 0.99f);
    if (left < 0) left = 0;
    snprintf(line, sizeof line, "%d:%02d", left / 60, left % 60);
    vp_color(d, left <= 15 ? 255 : 180, left <= 15 ? 80 : 180, left <= 15 ? 80 : 180);
    text(d, line, W - 120.0f, 3730, 100, VP_TEXT_RIGHT);
    snprintf(line, sizeof line, "%d", G.score);
    vp_color(d, 180, 180, 180);
    text(d, line, W / 2.0f, 3730, 85, VP_TEXT_CENTER);    /* under the top line: clear of long names */
}

static void draw_title(vp_dvg *d)
{
    char line[64];
    float t = G.state_time;
    plain();
    /* every 15 s: the title for 9, the high scores for 6 */
    float phase = fmodf(t < 0 ? t + 15 : t, 15.0f);
    if (t < 0 || phase >= 9.0f) {
        vp_color(d, 0, 200, 255);
        text(d, "BEST SHOWS", W / 2.0f, 3300, 150, VP_TEXT_CENTER);
        vp_color(d, 255, 255, 255);
        if (G.table_count) vp_hiscore_draw(d, G.table, G.table_count, W / 2.0f, 2900, 100, G.last_rank,
                                          G.time);
        else text(d, "NO SHOWS YET", W / 2.0f, 2500, 100, VP_TEXT_CENTER);
    } else {
        big(d, "BEAN", 3200, 430, WHITE, PINK_BORDER);
        big(d, "RUSH", 2650, 430, 255, 230, 0, 255, 120, 0);
        plain();
        vp_color(d, 0, 200, 255);
        text(d, "10 BEANS  3 RACES  1 CROWN", W / 2.0f, 2400, 95, VP_TEXT_CENTER);
    }
    if ((int)(t * 2) % 2 == 0) {
        vp_color(d, 255, 255, 255);
        if (G.credits.free_play || G.credits.credits > 0) text(d, "PRESS START", W / 2.0f, 700, 130, VP_TEXT_CENTER);
        else text(d, "INSERT COIN", W / 2.0f, 700, 130, VP_TEXT_CENTER);
    }
    vp_color(d, 110, 110, 130);
    text(d, "MUSIC: H.A. SODERBERG", W / 2.0f, 130, 45, VP_TEXT_CENTER);
    vp_color(d, 160, 160, 160);
    if (!G.credits.free_play) {
        snprintf(line, sizeof line, "CREDITS %d", G.credits.credits);
        text(d, line, W / 2.0f, 380, 80, VP_TEXT_CENTER);
    } else {
        text(d, "FREE PLAY", W / 2.0f, 380, 80, VP_TEXT_CENTER);
    }
}

static void draw_controls(vp_dvg *d)
{
    char spelled[VP_CONTROLS_LENGTH * 2];
    const char *one, *two;
    /* the fonts of the other games' controls pages: Asteroids for the
     * headings, Hershey for the controls and the help */
    plain();
    vp_text_font(VP_FONT_ASTEROIDS);
    vp_color(d, 0, 200, 255);
    text(d, "CONTROLS", W / 2.0f, 3550, 160, VP_TEXT_CENTER);
    for (int i = 0; i <= FUNCTIONS; i++) {
        int sel = i == G.ctl_row;
        float y = 3150 - i * 260.0f - (i == FUNCTIONS ? 80.0f : 0.0f);
        vp_text_font(VP_FONT_ASTEROIDS);
        if (sel) vp_color(d, 255, 60, 60); else vp_color(d, 255, 255, 255);
        if (i == FUNCTIONS) { text(d, "EXIT", W / 2.0f, y, 90, VP_TEXT_CENTER); break; }
        text(d, FUNCTION_LABEL[i], 400, y, 85, VP_TEXT_LEFT);
        if (sel && G.capture.capturing) {
            if ((int)(G.state_time * 3) % 2 == 0) { vp_color(d, 255, 220, 60); text(d, "...", 1900, y, 85, VP_TEXT_LEFT); }
            continue;
        }
        const char *has = vp_input_controls(G.in, FUNCTION_NAME[i]);
        vp_input_spelled(G.in, FUNCTION_NAME[i], has, spelled, sizeof spelled);
        vp_text_font(VP_FONT_HERSHEY);
        if (has && !strcmp(has, "standard")) vp_color(d, 150, 150, 150);
        else if (!sel) vp_color(d, 120, 255, 120);
        text(d, spelled, 1900, y, 75, VP_TEXT_LEFT);
    }
    vp_text_font(VP_FONT_HERSHEY);
    if (G.ctl_said[0]) { vp_color(d, 255, 220, 60); text(d, G.ctl_said, W / 2.0f, 1050, 70, VP_TEXT_CENTER); }
    vp_color(d, 160, 160, 160);
    if (G.ctl_row < FUNCTIONS || G.capture.capturing) {
        vp_capture_help(&G.capture, &one, &two);
        text(d, one, W / 2.0f, 850, 70, VP_TEXT_CENTER);
        text(d, two, W / 2.0f, 730, 70, VP_TEXT_CENTER);
    } else {
        text(d, "START, ENTER OR TAB - BACK TO THE TITLE", W / 2.0f, 790, 70, VP_TEXT_CENTER);
    }
    plain();
}

static void draw_center(vp_dvg *d)
{
    char line[64];
    plain();
    switch (G.state) {
    case ST_INTRO:
        snprintf(line, sizeof line, "ROUND %d", G.round + 1);
        big(d, G.round == 2 ? "FINAL" : line, 2950, 230, 0, 220, 255, 0, 90, 255);
        big(d, G.course.name, 2500, 200, WHITE, PINK_BORDER);
        plain();
        vp_color(d, 255, 255, 255);
        if (G.round == 2) text(d, "FIRST HOME WINS THE CROWN", W / 2.0f, 2250, 100, VP_TEXT_CENTER);
        else {
            snprintf(line, sizeof line, "%d BEANS  %d GO THROUGH", FIELD[G.round], QUALIFY[G.round]);
            text(d, line, W / 2.0f, 2250, 100, VP_TEXT_CENTER);
        }
        break;
    case ST_COUNTDOWN: {
        int n = 3 - (int)G.state_time;
        float f = G.state_time - (int)G.state_time;
        snprintf(line, sizeof line, "%d", n);
        big(d, line, 2000, 700 * (1.2f - 0.4f * f), 255, 230, 0, 255, 120, 0);
        break;
    }
    case ST_RACE:
        if (G.state_time < 1.0f)
            big(d, "GO!", 2100, 600, 120, 255, 80, 0, 140, 40);
        if (G.player_through > 0) {
            if (G.round == 2) big(d, "YOU WIN!", 2500, 320, 255, 230, 0, 255, 120, 0);
            else big(d, "THROUGH!", 2500, 320, WHITE, PINK_BORDER);
        } else if (G.beans[G.player].fallen > 0) {
            big(d, "BACK TO THE", 2600, 190, WHITE, 255, 40, 40);
            big(d, "CHECKPOINT", 2300, 190, WHITE, 255, 40, 40);
        }
        break;
    case ST_RESULT:
        if (G.qualified) {
            if (G.round == 2) big(d, "CROWN WON!", 2650, 300, 255, 230, 0, 255, 120, 0);
            else big(d, "QUALIFIED!", 2650, 300, WHITE, PINK_BORDER);
            plain();
            vp_color(d, 255, 255, 255);
            snprintf(line, sizeof line, "%s PLACE   +%d", ordinal(G.beans[G.player].place), G.round_points);
            text(d, line, W / 2.0f, 2350, 120, VP_TEXT_CENTER);
        } else {
            big(d, "ELIMINATED", 2650, 300, WHITE, 255, 40, 40);
        }
        break;
    case ST_SHOW_OVER:
        if (G.won) big(d, "CHAMPION!", 2850, 330, 255, 230, 0, 255, 120, 0);
        else big(d, "SHOW OVER", 2850, 330, WHITE, 0, 90, 255);
        plain();
        vp_color(d, 255, 255, 255);
        if (!G.won) {
            snprintf(line, sizeof line, "OUT IN ROUND %d", G.best_round + 1);
            text(d, line, W / 2.0f, 2550, 110, VP_TEXT_CENTER);
        }
        snprintf(line, sizeof line, "SCORE %d", G.score);
        text(d, line, W / 2.0f, 2250, 140, VP_TEXT_CENTER);
        break;
    case ST_INITIALS:
        big(d, "A BEST SHOW!", 2900, 230, WHITE, PINK_BORDER);
        plain();
        vp_color(d, 255, 255, 255);
        snprintf(line, sizeof line, "SCORE %d", G.score);
        text(d, line, W / 2.0f, 2550, 120, VP_TEXT_CENTER);
        vp_color(d, 0, 255, 255);
        vp_initials_draw(&G.entry, d, W / 2.0f, 1900, 260);
        vp_color(d, 160, 160, 160);
        text(d, "STICK UP DOWN - BUTTON 1 NEXT", W / 2.0f, 1400, 80, VP_TEXT_CENTER);
        break;
    default:
        break;
    }
}

static void draw(vp_game *g, vp_dvg *d)
{
    (void)g;
    /* the lettering first: the big letters it draws hide the world behind them */
    g_nbig = 0;
    vp3d_look_at(&G.view, G.cam_eye, G.cam_at, vp_v3(0, 1, 0));
    switch (G.state) {
    case ST_TITLE:    draw_title(d); break;
    case ST_CONTROLS: draw_controls(d); break;
    case ST_RACE:
    case ST_COUNTDOWN:
    case ST_RESULT:   draw_hud(d); draw_center(d); break;
    default:          draw_center(d); break;
    }
    if (G.state != ST_CONTROLS) {
        int marked = (G.state == ST_TITLE || G.state == ST_INTRO) ? -1 : G.player;
        render_world(d, &G.view, &G.course, G.clock, G.beans, G.nbeans, marked, G.crowned,
                     g_nbig ? hide_lettering : NULL);
    }
}

/* ---- start and end -------------------------------------------------------------------- */

static int init(vp_game *g)
{
    G.in = vp_game_input(g);
    G.audio = vp_game_audio(g);
    define_controls(G.in);
    vp_credits_init(&G.credits);
    load_sounds();
    char path[1024];
    if (G.audio && !music_open(vp_asset_path("data/music.dat", path, sizeof path)))
        fprintf(stderr, "beanrush: cannot read %s: no music\n", path);
    if (!render_init()) {
        fprintf(stderr, "beanrush: %s\n", vp_last_error());
        return VP_ERR_NOMEM;
    }
    vp3d_view_init(&G.view, 72.0f);
    G.view.far_z = 70.0f;
    G.view.fog_start = 18.0f;
    G.view.fog_end = 65.0f;
    G.view.fog_floor = 0.2f;
    G.table_count = vp_hiscore_load(G.table);
    G.last_rank = -1;
    G.rng = 0xbea05u;
    G.autoplay = getenv("BEANRUSH_AUTOPLAY") && atoi(getenv("BEANRUSH_AUTOPLAY"));
    G.facecam = getenv("BEANRUSH_FACECAM") && atoi(getenv("BEANRUSH_FACECAM"));
    G.shots = getenv("BEANRUSH_SHOTS");
    G.shot_every = getenv("BEANRUSH_SHOT_EVERY") ? atof(getenv("BEANRUSH_SHOT_EVERY")) : 0.5;
    if (G.shot_every <= 0) G.shot_every = 0.5;
    G.next_shot = getenv("BEANRUSH_SHOT_FROM") ? atof(getenv("BEANRUSH_SHOT_FROM")) : 0;
    start_demo();
    set_state(ST_TITLE);
    return VP_OK;
}

static void shutdown(vp_game *g)
{
    (void)g;
    props_free();
    render_free();
    music_close();
}

int main(int argc, char **argv)
{
    vp_game_desc desc = {0};
    desc.title = "Bean Rush";
    desc.init = init;
    desc.update = update;
    desc.draw = draw;
    desc.shutdown = shutdown;
    desc.frame_hz_board = 30;       /* the board gets more time per frame (40 in a window) */
    return vp_run(&desc, argc, argv);
}

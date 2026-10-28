/* The music archive, data/music.dat (made by tools/pack_music.py): the
 * tracks packed and scrambled, so they are not loose .ogg files a player
 * could copy out of the game (their licence forbids it). Read whole at
 * start; a track is unscrambled into a buffer when it is played. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beanrush.h"

#define KEY  0x6A3B5E21u
#define NAME 40

static unsigned char *g_dat;
static size_t g_size;
static unsigned g_count;

static unsigned get32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static void unscramble(unsigned char *p, size_t n, unsigned offset)
{
    unsigned s = KEY + offset;
    if (!s) s = 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        p[i] ^= (unsigned char)s;
    }
}

int music_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = n > 8 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n || memcmp(d, "BRM1", 4) != 0) {
        free(d);
        fclose(f);
        return 0;
    }
    fclose(f);
    unsigned count = get32(d + 4);
    size_t table = (size_t)count * (8 + NAME);
    if (8 + table > (size_t)n) { free(d); return 0; }
    unscramble(d + 8, table, 8);
    g_dat = d;
    g_size = (size_t)n;
    g_count = count;
    return 1;
}

unsigned char *music_track(const char *name, size_t *size)
{
    for (unsigned i = 0; i < g_count; i++) {
        const unsigned char *e = g_dat + 8 + i * (8 + NAME);
        if (strncmp((const char *)e + 8, name, NAME) != 0) continue;
        unsigned at = get32(e), n = get32(e + 4);
        if ((size_t)at + n > g_size) return NULL;
        unsigned char *t = malloc(n ? n : 1);
        if (!t) return NULL;
        memcpy(t, g_dat + at, n);
        unscramble(t, n, at);
        *size = n;
        return t;
    }
    return NULL;
}

void music_close(void)
{
    free(g_dat);
    g_dat = NULL;
    g_size = g_count = 0;
}

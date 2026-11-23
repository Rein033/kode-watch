/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kody.c
 * @brief Kody's player: see kody.h and tools/kodyframes.py for the blob.
 */
#include "esp_attr.h"
#include "kody.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kk.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "miniz.h"
/* The inflater's state is ~11 KB: kept here, never on a task's stack. */
static tinfl_decompressor s_inf;   /* internal: inflated every frame, PSRAM is too slow */
static size_t inflate_to(void *out, size_t cap, const void *src, size_t len)
{
    size_t in_len = len, out_len = cap;
    tinfl_init(&s_inf);
    tinfl_status const st = tinfl_decompress(&s_inf, (const mz_uint8 *)src, &in_len, (mz_uint8 *)out, (mz_uint8 *)out,
                                             &out_len, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return (TINFL_STATUS_DONE == st) ? out_len : 0;
}
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define KLOG(...) ESP_LOGW("kody", __VA_ARGS__)
#else
#define KLOG(...) ((void)0)
#include <zlib.h>
static size_t inflate_to(void *out, size_t cap, const void *src, size_t len)
{
    uLongf n = cap;
    return (Z_OK == uncompress(out, &n, src, len)) ? n : 0;
}
#define BIG_ALLOC(n) malloc(n)
#endif

typedef struct {
    uint16_t n, fps;
    uint32_t first;
} scene_t;

#define NSCENES 6               /* per world, in kody_scene_t's order */
#define MAXWORLDS 12
static const uint8_t *s_blob;
static const uint32_t *s_offs;
static scene_t s_sc[NSCENES * MAXWORLDS];
static char s_wname[MAXWORLDS][17];
static int s_nworlds, s_world;
static uint16_t *s_pic;        /* Kody's picture, kept between frames: rows may repeat */
static uint8_t *s_raw;         /* one frame, inflated */
#define RAW_CAP (KODY_H * (2 + 4 * 64) + 64 * 1024)
static kody_scene_t s_cur = KODY_LOOP;
static int s_frame = -1;
static uint32_t s_t0;
static bool s_ok;
static const char *s_why = "not started";
static int s_fail_count;
const char *kody_status(void) { return (s_ok && (0 == s_fail_count)) ? "ok" : (s_ok ? "inflate failed" : s_why); }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

bool kody_init(const uint8_t *blob)
{
#ifdef ESP_PLATFORM
    if ( NULL == blob )
    {
        const esp_partition_t *const part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "kody");
        const void *map = NULL;
        esp_partition_mmap_handle_t h;
        if ( NULL == part ) { KLOG("no kody partition"); s_why = "no partition"; }
        else
        {
            esp_err_t const e = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h);
            KLOG("partition at 0x%lx, %lu bytes, mmap %s", (unsigned long)part->address, (unsigned long)part->size, esp_err_to_name(e));
            if ( ESP_OK == e ) { blob = map; }
            else { s_why = "mmap failed"; }
        }
    }
#else
    if ( (NULL == blob) && getenv("KODY_BLOB") )
    {
        /* the host render: the blob from a file */
        FILE *const f = fopen(getenv("KODY_BLOB"), "rb");
        if ( f )
        {
            (void)fseek(f, 0, SEEK_END);
            long const n = ftell(f);
            (void)fseek(f, 0, SEEK_SET);
            uint8_t *const m = malloc((size_t)n);
            if ( m && (fread(m, 1, (size_t)n, f) == (size_t)n) ) { blob = m; }
            (void)fclose(f);
        }
    }
#endif
    if ( NULL == blob ) { return false; }
    uint16_t const nsc = (0 == memcmp(blob, "KODZ", 4)) ? rd16(blob + 4) : 0;
    if ( (nsc < NSCENES) || (0 != (nsc % NSCENES)) || (nsc > NSCENES * MAXWORLDS) )
    {
        KLOG("no Kody frames in the partition (starts %02x %02x %02x %02x)", blob[0], blob[1], blob[2], blob[3]);
        s_why = "partition empty";
        return false;
    }
    const uint8_t *p = blob + 6;
    s_nworlds = nsc / NSCENES;
    for ( int i = 0; i < nsc; ++i, p += 24 )
    {
        if ( 0 == (i % NSCENES) ) { memcpy(s_wname[i / NSCENES], p, 16); s_wname[i / NSCENES][16] = '\0'; }
        s_sc[i].n = rd16(p + 16);
        s_sc[i].fps = rd16(p + 18);
        s_sc[i].first = rd32(p + 20);
    }
    s_offs = (const uint32_t *)(p + 4);
    s_blob = blob;
    s_pic = BIG_ALLOC((size_t)KODY_W * KODY_H * 2);
    s_raw = BIG_ALLOC(RAW_CAP);
    s_ok = (NULL != s_pic) && (NULL != s_raw);
    if ( !s_ok ) { s_why = "no memory"; }
    KLOG("ready: %s, %d worlds, loop %u frames at %u fps", s_ok ? "yes" : "no memory", s_nworlds, s_sc[0].n, s_sc[0].fps);
    if ( s_ok ) { memset(s_pic, 0, (size_t)KODY_W * KODY_H * 2); }
    return s_ok;
}

bool kody_ready(void) { return s_ok; }

int kody_worlds(void) { return s_nworlds; }
const char *kody_world(void) { return s_ok ? s_wname[s_world] : "-"; }

static int world_index(const char *name)
{
    for ( int i = 0; i < s_nworlds; ++i ) { if ( 0 == strcmp(s_wname[i], name) ) { return i; } }
    return -1;
}

void kody_set_hour(int h)
{
    /* kodeOS's scenes of the hour (kgui_ao.c): one at random among those this
     * blob has, the same until the hour turns; the first world otherwise. */
    static int last = -1;
    if ( !s_ok || (h < 0) || (h == last) ) { return; }
    last = h;
    static const char *const MORNING[] = { "desayuno", "tren", "parque" };
    static const char *const MIDDAY[] = { "tren", "parque" };
    static const char *const AFTERNOON[] = { "taller", "parque", "tren" };
    static const char *const EVENING[] = { "playa", "azotea", "camping" };
    static const char *const NIGHT[] = { "madrugada", "camping", "azotea" };
    const char *const *list = NIGHT;
    int n = 3;
    if ( (h >= 7) && (h < 10) ) { list = MORNING; }
    else if ( (h >= 10) && (h < 16) ) { list = MIDDAY; n = 2; }
    else if ( (h >= 16) && (h < 20) ) { list = AFTERNOON; }
    else if ( (h >= 20) && (h < 22) ) { list = EVENING; }
    int pool[3], k = 0;
    for ( int i = 0; i < n; ++i )
    {
        int const w = world_index(list[i]);
        if ( w >= 0 ) { pool[k++] = w; }
    }
    int const next = (k > 0) ? pool[kk_rand(k)] : 0;
    if ( next != s_world )
    {
        s_world = next;
        s_cur = KODY_WAKE;      /* a new world comes in with its own wake-up */
        s_frame = -1;
        s_t0 = kk_ms();
    }
}

void kody_play(kody_scene_t s)
{
    if ( !s_ok || (s == s_cur) ) { return; }
    s_cur = s;
    s_frame = -1;
    s_t0 = kk_ms();
}

static void decode(uint32_t idx)
{
    uint32_t const a = s_offs[idx], b = s_offs[idx + 1];
    size_t const n = inflate_to(s_raw, RAW_CAP, s_blob + a, b - a);
    if ( 0 == n )
    {
        if ( s_fail_count++ < 3 ) { KLOG("frame %lu does not inflate (%lu bytes at 0x%lx)", (unsigned long)idx, (unsigned long)(b - a), (unsigned long)a); }
        return;
    }
    const uint8_t *p = s_raw, *const end = s_raw + n;
    for ( int y = 0; (y < KODY_H) && (p + 2 <= end); ++y )
    {
        uint16_t const runs = rd16(p);
        p += 2;
        if ( 0xFFFF == runs ) { continue; }
        uint16_t *row = &s_pic[y * KODY_W];
        int x = 0;
        for ( uint16_t r = 0; (r < runs) && (p + 4 <= end); ++r, p += 4 )
        {
            int cnt = rd16(p);
            uint16_t const c = rd16(p + 2);
            if ( x + cnt > KODY_W ) { cnt = KODY_W - x; }
            for ( int k = 0; k < cnt; ++k ) { row[x + k] = c; }
            x += cnt;
        }
    }
}

void kody_draw(uint16_t *fb, int y0, uint32_t now)
{
    if ( !s_ok ) { return; }
    if ( s_frame < 0 ) { s_t0 = now; }
    scene_t const *sc = &s_sc[s_world * NSCENES + s_cur];
    int f = (int)(((now - s_t0) * sc->fps) / 1000U);
    if ( f >= sc->n )
    {
        if ( KODY_LOOP == s_cur ) { f %= sc->n; }
        else
        {
            s_cur = KODY_LOOP;
            s_t0 = now;
            s_frame = -1;
            f = 0;
            sc = &s_sc[s_world * NSCENES + KODY_LOOP];
        }
    }
    /* Frames build on the ones before: walk from the last one shown (or the scene's start). */
    int from = (s_frame < 0 || f < s_frame) ? 0 : s_frame + 1;
    for ( int i = from; i <= f; ++i ) { decode(sc->first + (uint32_t)i); }
    s_frame = f;
    for ( int y = 0; y < KODY_H; ++y )
    {
        int const yy = y0 + y;
        if ( (yy < 0) || (yy >= KK_H) ) { continue; }
        memcpy(&fb[yy * KK_W], &s_pic[y * KODY_W], KODY_W * 2);
    }
}

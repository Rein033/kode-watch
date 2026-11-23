/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kk.c
 * @brief kodekit: whole frames into the panel's back buffer, flipped through
 *        AO_Display; buttons from AO_Buttons; touch from the kode layer.
 */
#include "esp_attr.h"
#include "kk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "kfont.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "font5x7.h"

static uint16_t *s_fb;
static int s_which;
static bool s_in_flight;
static uint32_t s_held, s_prev;
static bool s_touch, s_touch_prev;
static int s_tx, s_ty;
static uint32_t s_rng = 0x2545F491U;
static uint32_t s_chord_since;

/* The link lives in the board layer: the sim's bridge now, BLE on hardware.
 * Weak, so an app on a board without it still links (the link stays down). */
extern void kdotsim_link_send(const char *msg, size_t len) __attribute__((weak));
extern int kdotsim_link_recv(char *buf, size_t cap) __attribute__((weak));
extern bool kdotsim_mirror(uint16_t *dst, uint16_t *w, uint16_t *h, size_t cap_px) __attribute__((weak));

#define KK_LQ 64   /* the hub sends a burst on connect (rooms, weather, agenda) */
#define KK_LLEN 512
EXT_RAM_BSS_ATTR static char s_lq[KK_LQ][KK_LLEN];
static unsigned s_lq_head, s_lq_tail;
static uint32_t s_rpulse;                 /* remote button presses for this frame */
static bool s_rtouch;
static int s_rtx, s_rty;
static bool s_link_up;
static int64_t s_epoch_at;                /* local wall time (s) at s_epoch_ms */
static uint32_t s_epoch_ms;
static bool s_epoch_ok;

static void link_poll(void)
{
    if ( NULL == kdotsim_link_recv ) { return; }
    char m[KK_LLEN];
    while ( kdotsim_link_recv(m, sizeof(m)) >= 0 )
    {
        if ( 0 == strncmp(m, "rb ", 3) )
        {
            s_rpulse |= (uint32_t)strtoul(m + 3, NULL, 10);
        }
        else if ( 0 == strncmp(m, "rt ", 3) )
        {
            int d = 0, x = 0, y = 0;
            if ( sscanf(m + 3, "%d %d %d", &d, &x, &y) == 3 )
            {
                s_rtouch = (d != 0);
                s_rtx = x;
                s_rty = y;
            }
        }
        else if ( 0 == strncmp(m, "sys time ", 9) )
        {
            long long e = 0, off = 0;
            if ( sscanf(m + 9, "%lld %lld", &e, &off) == 2 )
            {
                s_epoch_at = (int64_t)(e + off);
                s_epoch_ms = board_ms();
                s_epoch_ok = true;
                board_rtc_set(s_epoch_at);
            }
        }
        else
        {
            if ( 0 == strncmp(m, "hub peer ", 9) ) { s_link_up = ('1' == m[9]); }
            if ( (s_lq_head - s_lq_tail) < KK_LQ )
            {
                (void)snprintf(s_lq[s_lq_head % KK_LQ], KK_LLEN, "%s", m);
                ++s_lq_head;
            }
        }
    }
}

bool kk_link_send(const char *fmt, ...)
{
    if ( NULL == kdotsim_link_send ) { return false; }
    char m[KK_LLEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    if ( n < 0 ) { return false; }
    if ( n >= (int)sizeof(m) ) { n = (int)sizeof(m) - 1; }
    for ( int i = 0; i < n; ++i ) { if ( ('\n' == m[i]) || ('\r' == m[i]) ) { m[i] = ' '; } }
    kdotsim_link_send(m, (size_t)n);
    return true;
}

bool kk_link_recv(char *buf, int cap)
{
    if ( s_lq_tail == s_lq_head ) { return false; }
    (void)snprintf(buf, (size_t)cap, "%s", s_lq[s_lq_tail % KK_LQ]);
    ++s_lq_tail;
    return true;
}

bool kk_link_up(void) { return s_link_up; }

bool kk_mirror(uint16_t *dst, int *w, int *h)
{
    if ( NULL == kdotsim_mirror ) { return false; }
    uint16_t mw = 0, mh = 0;
    if ( !kdotsim_mirror(dst, &mw, &mh, (size_t)KK_MIRROR_W * KK_MIRROR_H) ) { return false; }
    *w = mw;
    *h = mh;
    return true;
}

bool kk_clock(kk_clock_t *c)
{
    if ( !s_epoch_ok ) { return false; }
    time_t const t = (time_t)(s_epoch_at + (int64_t)((board_ms() - s_epoch_ms) / 1000U));
    struct tm tm;
    (void)gmtime_r(&t, &tm);
    c->year = tm.tm_year + 1900;
    c->mon = tm.tm_mon + 1;
    c->mday = tm.tm_mday;
    c->wday = tm.tm_wday;
    c->hour = tm.tm_hour;
    c->min = tm.tm_min;
    c->sec = tm.tm_sec;
    return true;
}

void kk_set_local_time(int64_t local_epoch)
{
    s_epoch_at = local_epoch;
    s_epoch_ms = board_ms();
    s_epoch_ok = true;
    board_rtc_set(local_epoch);
}

void kk_blit_rect(int x, int y, int w, int h, const uint16_t *src)
{
    if ( (NULL == s_fb) || (NULL == src) ) { return; }
    for ( int r = 0; r < h; ++r )
    {
        int const yy = y + r;
        if ( (yy < 0) || (yy >= KK_H) ) { continue; }
        for ( int i = 0; i < w; ++i )
        {
            int const xx = x + i;
            if ( (xx >= 0) && (xx < KK_W) ) { s_fb[yy * KK_W + xx] = src[r * w + i]; }
        }
    }
}

void kk_init(void)
{
    esp_err_t e = nvs_flash_init();
    if ( (ESP_ERR_NVS_NO_FREE_PAGES == e) || (ESP_ERR_NVS_NEW_VERSION_FOUND == e) )
    {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }
    board_init();
    int64_t t = 0;
    if ( board_rtc_get(&t) )
    {
        s_epoch_at = t;
        s_epoch_ms = board_ms();
        s_epoch_ok = true;
    }
    s_rng ^= (uint32_t)board_ms() * 2654435761U;
}

uint16_t *kk_begin(void)
{
    s_fb = board_fb(s_which);     /* the buffer that is not being flushed */
    (void)s_in_flight;
    link_poll();
    s_prev = s_held;
    s_held = board_buttons() | s_rpulse;
    s_rpulse = 0U;
    s_touch_prev = s_touch;
    int x = 0, y = 0;
    s_touch = board_touch(&x, &y);
    if ( s_touch )
    {
        s_tx = x;
        s_ty = y;
    }
    else if ( s_rtouch )
    {
        s_touch = true;
        s_tx = s_rtx;
        s_ty = s_rty;
    }
    return s_fb;
}

__attribute__((weak)) void kk_frame_hook(const uint16_t *fb) { (void)fb; }

void kk_end(void)
{
    if ( NULL == s_fb )
    {
        vTaskDelay(1);
        return;
    }
    kk_frame_hook(s_fb);
    board_flush(s_fb);
    s_which ^= 1;   /* the next frame goes in the other buffer */
    (void)s_chord_since;
    vTaskDelay(1);   /* let the idle task have its turn */
}

/* ---- drawing ---- */

void kk_pixel(int x, int y, uint16_t c)
{
    if ( (NULL != s_fb) && ((unsigned)x < KK_W) && ((unsigned)y < KK_H) )
    {
        s_fb[y * KK_W + x] = c;
    }
}

void kk_rect(int x, int y, int w, int h, uint16_t c)
{
    if ( NULL == s_fb ) { return; }
    if ( x < 0 ) { w += x; x = 0; }
    if ( y < 0 ) { h += y; y = 0; }
    if ( x + w > KK_W ) { w = KK_W - x; }
    if ( y + h > KK_H ) { h = KK_H - y; }
    if ( (w <= 0) || (h <= 0) ) { return; }
    for ( int r = 0; r < h; ++r )
    {
        uint16_t *p = &s_fb[(y + r) * KK_W + x];
        for ( int i = 0; i < w; ++i ) { p[i] = c; }
    }
}

void kk_blit(const uint16_t *src)
{
    if ( (NULL != s_fb) && (NULL != src) ) { memcpy(s_fb, src, sizeof(uint16_t) * KK_W * KK_H); }
}

void kk_clear(uint16_t c)
{
    kk_rect(0, 0, KK_W, KK_H, c);
}

void kk_box(int x, int y, int w, int h, int t, uint16_t c)
{
    kk_rect(x, y, w, t, c);
    kk_rect(x, y + h - t, w, t, c);
    kk_rect(x, y, t, h, c);
    kk_rect(x + w - t, y, t, h, c);
}

void kk_disc(int cx, int cy, int r, uint16_t c)
{
    for ( int dy = -r; dy <= r; ++dy )
    {
        int dx = 0;
        while ( (dx + 1) * (dx + 1) + dy * dy <= r * r ) { ++dx; }
        kk_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

void kk_ring(int cx, int cy, int r, int t, uint16_t c)
{
    int const r2 = r * r, i2 = (r - t) * (r - t);
    for ( int dy = -r; dy <= r; ++dy )
    {
        for ( int dx = -r; dx <= r; ++dx )
        {
            int const d = dx * dx + dy * dy;
            if ( (d <= r2) && (d > i2) ) { kk_pixel(cx + dx, cy + dy, c); }
        }
    }
}

void kk_round(int x, int y, int w, int h, int r, uint16_t c)
{
    if ( r * 2 > w ) { r = w / 2; }
    if ( r * 2 > h ) { r = h / 2; }
    kk_rect(x, y + r, w, h - 2 * r, c);
    for ( int dy = 0; dy < r; ++dy )
    {
        int const yy = r - dy;
        int dx = 0;
        while ( (dx + 1) * (dx + 1) + yy * yy <= r * r ) { ++dx; }
        int const inset = r - dx;
        kk_rect(x + inset, y + dy, w - 2 * inset, 1, c);
        kk_rect(x + inset, y + h - 1 - dy, w - 2 * inset, 1, c);
    }
}

void kk_line(int x0, int y0, int x1, int y1, int t, uint16_t c)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int const h = t / 2;
    for ( ;; )
    {
        if ( t <= 1 ) { kk_pixel(x0, y0, c); }
        else { kk_rect(x0 - h, y0 - h, t, t, c); }
        if ( (x0 == x1) && (y0 == y1) ) { break; }
        int const e2 = 2 * err;
        if ( e2 >= dy ) { err += dy; x0 += sx; }
        if ( e2 <= dx ) { err += dx; y0 += sy; }
    }
}

uint16_t kk_mix(uint16_t a, uint16_t b, int t)
{
    int const ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int const br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int const r = ar + ((br - ar) * t) / 256;
    int const g = ag + ((bg - ag) * t) / 256;
    int const bl = ab + ((bb - ab) * t) / 256;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void kk_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom)
{
    for ( int r = 0; r < h; ++r )
    {
        kk_rect(x, y + r, w, 1, kk_mix(top, bottom, (r * 256) / ((h > 1) ? (h - 1) : 1)));
    }
}

int kk_text_w(const char *str, int s)
{
    return kf_compat_w(str, s);
    int const n = (int)strlen(str);
    return (n > 0) ? (n * 6 * s - s) : 0;
}

int kk_text(int x, int y, const char *str, int s, uint16_t c)
{
    if ( s_fb ) { return kf_compat(s_fb, x, y, str, s, c); }
    int const x0 = x;
    for ( const char *p = str; *p; ++p )
    {
        int ch = (unsigned char)*p;
        if ( (ch < 32) || (ch > 126) ) { ch = '?'; }
        const uint8_t *gl = KK_FONT[ch - 32];
        for ( int r = 0; r < 7; ++r )
        {
            for ( int col = 0; col < 5; ++col )
            {
                if ( gl[r] & (0x10 >> col) )
                {
                    kk_rect(x + col * s, y + r * s, s, s, c);
                }
            }
        }
        x += 6 * s;
    }
    return x - x0;
}

void kk_text_c(int y, const char *str, int s, uint16_t c)
{
    kk_text((KK_W - kk_text_w(str, s)) / 2, y, str, s, c);
}

void kk_num_c(int y, long n, int s, uint16_t c)
{
    char buf[24];
    (void)snprintf(buf, sizeof(buf), "%ld", n);
    kk_text_c(y, buf, s, c);
}

/* ---- input ---- */

uint32_t kk_held(void) { return s_held; }
bool kk_pressed(uint32_t bit) { return ((s_held & bit) != 0U) && ((s_prev & bit) == 0U); }
bool kk_released(uint32_t bit) { return ((s_held & bit) == 0U) && ((s_prev & bit) != 0U); }

bool kk_touch(int *x, int *y)
{
    if ( s_touch )
    {
        if ( x ) { *x = s_tx; }
        if ( y ) { *y = s_ty; }
    }
    return s_touch;
}

bool kk_tapped(int *x, int *y)
{
    if ( s_touch && !s_touch_prev )
    {
        if ( x ) { *x = s_tx; }
        if ( y ) { *y = s_ty; }
        return true;
    }
    return false;
}

/* ---- the rest ---- */

uint32_t kk_ms(void) { return board_ms(); }

uint32_t kk_rand(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (0U == n) ? 0U : (s_rng % n);
}

void kk_tone(uint16_t hz, uint16_t ms) { (void)hz; (void)ms; }   /* ES8311 speaker: later */
void kk_led(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }   /* the watch has no LED */
void kk_buzz(uint16_t ms) { (void)ms; }   /* DRV2605L: later */
int kk_battery(void) { return board_battery(); }
bool kk_power_in(void) { return board_power_in(); }
bool kk_charging(void) { return board_charging(); }
void kk_bright(int pct) { board_brightness(pct); }
void kk_skip(void) { vTaskDelay(pdMS_TO_TICKS(40)); }   /* a frame without a flush; the screen keeps its picture */

long kk_best_get(const char *key)
{
    uint64_t v = 0U;
    nvs_handle_t h;
    if ( ESP_OK != nvs_open("kodekit", NVS_READONLY, &h) ) { return 0; }
    if ( ESP_OK != nvs_get_u64(h, key, &v) ) { v = 0U; }
    nvs_close(h);
    return (long)v;
}

bool kk_blob_get(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    if ( ESP_OK != nvs_open("kodekit", NVS_READONLY, &h) ) { return false; }
    bool const ok = (ESP_OK == nvs_get_blob(h, key, buf, len));
    nvs_close(h);
    return ok;
}

void kk_blob_put(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if ( ESP_OK != nvs_open("kodekit", NVS_READWRITE, &h) ) { return; }
    (void)nvs_set_blob(h, key, buf, len);
    (void)nvs_commit(h);
    nvs_close(h);
}

void kk_best_put(const char *key, long v)
{
    nvs_handle_t h;
    if ( ESP_OK != nvs_open("kodekit", NVS_READWRITE, &h) ) { return; }
    (void)nvs_set_u64(h, key, (uint64_t)v);
    (void)nvs_commit(h);
    nvs_close(h);
}

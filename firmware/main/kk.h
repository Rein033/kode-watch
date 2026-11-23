/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kk.h
 * @brief kodekit: a small drawing and input kit for Kode Dot apps that paint
 *        whole frames themselves (no LVGL), on the public kode SDK.
 *
 * The app's loop is: kk_begin() (wait for the last flip, take the back
 * buffer), draw, kk_end() (flip). Input is read once per kk_begin().
 */
#ifndef KK_H_
#define KK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KK_W 410
#define KK_H 502

/* The glass has round corners (radius 114): keep text inside this band. */
#define KK_SAFE_X 40
#define KK_SAFE_Y 36

#define KK_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define KK_BLACK   KK_RGB(0, 0, 0)
#define KK_WHITE   KK_RGB(255, 255, 255)
#define KK_GREY    KK_RGB(120, 128, 140)
#define KK_DARK    KK_RGB(22, 26, 34)
#define KK_BLUE    KK_RGB(2, 138, 194)     /* TOP */
#define KK_RED     KK_RGB(201, 48, 61)     /* BOT */
#define KK_GREEN   KK_RGB(70, 190, 90)
#define KK_YELLOW  KK_RGB(250, 200, 40)
#define KK_ORANGE  KK_RGB(245, 130, 40)
#define KK_PURPLE  KK_RGB(150, 90, 220)
#define KK_CYAN    KK_RGB(60, 200, 220)
#define KK_PINK    KK_RGB(240, 110, 170)

/* Buttons, as kode_sys_button_read_mask() reports them. */
#define KK_UP    (1U << 0)
#define KK_DOWN  (1U << 1)
#define KK_LEFT  (1U << 2)
#define KK_RIGHT (1U << 3)
#define KK_TOP   (1U << 4)
#define KK_BOT   (1U << 5)

/** @brief Bring the board up (kode_init) and wait for the panel. */
void kk_init(void);

/** @brief Start a frame: wait for the last flip, read the inputs, return the buffer to draw in. */
uint16_t *kk_begin(void);

/** @brief Show the frame. Holding TOP and BOT together for a second leaves the app. */
void kk_end(void);
/** @brief Called with each finished frame, just before it is flushed (weak: the OS streams it). */
void kk_frame_hook(const uint16_t *fb);

/* ---- drawing, clipped to the glass ---- */
void kk_clear(uint16_t c);
void kk_rect(int x, int y, int w, int h, uint16_t c);
void kk_box(int x, int y, int w, int h, int t, uint16_t c);
void kk_round(int x, int y, int w, int h, int r, uint16_t c);
void kk_disc(int cx, int cy, int r, uint16_t c);
void kk_ring(int cx, int cy, int r, int t, uint16_t c);
void kk_line(int x0, int y0, int x1, int y1, int t, uint16_t c);
void kk_pixel(int x, int y, uint16_t c);
void kk_vgrad(int x, int y, int w, int h, uint16_t top, uint16_t bottom);
uint16_t kk_mix(uint16_t a, uint16_t b, int t256);
void kk_blit(const uint16_t *src);    /* a whole KK_W x KK_H picture */

/** @brief Text in the kit's 5x7 font, @p s pixels per font pixel. Returns the width drawn. */
int kk_text(int x, int y, const char *str, int s, uint16_t c);
int kk_text_w(const char *str, int s);
void kk_text_c(int y, const char *str, int s, uint16_t c);   /* centred */
void kk_num_c(int y, long n, int s, uint16_t c);

/* ---- input ---- */
uint32_t kk_held(void);                 /* buttons down now */
bool kk_pressed(uint32_t bit);          /* went down since the last frame */
bool kk_released(uint32_t bit);
bool kk_touch(int *x, int *y);          /* finger on the glass now */
bool kk_tapped(int *x, int *y);         /* finger landed since the last frame */

/* ---- the rest of the board ---- */
uint32_t kk_ms(void);
uint32_t kk_rand(uint32_t n);           /* 0 .. n-1 */
void kk_tone(uint16_t hz, uint16_t ms);
void kk_led(uint8_t r, uint8_t g, uint8_t b);
void kk_buzz(uint16_t ms);
long kk_best_get(const char *key);
void kk_best_put(const char *key, long v);
bool kk_blob_get(const char *key, void *buf, size_t *len);   /* *len: in the room, out what was read */
void kk_blob_put(const char *key, const void *buf, size_t len);
void kk_blit_rect(int x, int y, int w, int h, const uint16_t *src);

/* ---- Kode Link: text messages to the paired device ----
 * Between a Kode Dot and a Kode Watch this is BLE on real hardware; in the sim
 * the server's hub carries it. Messages are one line, at most 500 bytes.
 * kodekit handles a few itself, before the app sees them:
 *   "rb <bit>"           press a button for one frame (remote control)
 *   "rt <0|1> <x> <y>"   remote touch, in screen pixels
 *   "sys time <epoch> <utc-offset-s>"   the wall clock
 */
bool kk_link_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool kk_link_recv(char *buf, int cap);     /* next message for the app, false if none */
bool kk_link_up(void);                     /* the paired device is there */

/* A picture of the paired device's screen (Dot side), at most KK_MIRROR_W x KK_MIRROR_H. */
#define KK_MIRROR_W 205
#define KK_MIRROR_H 251
bool kk_mirror(uint16_t *dst, int *w, int *h);   /* true when a new one arrived */

/* Wall clock, from the link ("sys time"); false until it is known. */
typedef struct { int year, mon, mday, wday, hour, min, sec; } kk_clock_t;
bool kk_clock(kk_clock_t *c);
void kk_set_local_time(int64_t local_epoch);   /* wall time from elsewhere (the phone) */

/* ---- power (on the watch; the sim has none) ---- */
int kk_battery(void);      /* percent, -1 when unknown */
bool kk_power_in(void);    /* USB present: docked or on a cable */
bool kk_charging(void);
void kk_bright(int pct);
/** @brief Instead of kk_end(): no flush, the screen keeps the last frame (for the dark rest face). */
void kk_skip(void);

#endif /* KK_H_ */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kfont.c
 * @brief kodeOS's type and glyphs on the watch: 4-bit alpha maps blended into
 *        the RGB565 frame. kodekit's kk_text() comes here on the watch, so the
 *        old 5x7 sizes map onto kodeOS's roles (caption, body, title, figures).
 */
#include "kfont.h"

#include <string.h>

#include "kk.h"

static const kf_glyph_t *find(const kf_font_t *f, char ch)
{
    uint16_t const cp = (uint8_t)ch;
    /* The tables are in code-point order; most are ASCII from 32. */
    if ( (f->n > 0) && (f->g[0].cp == 32) && (cp >= 32) && ((cp - 32) < f->n) && (f->g[cp - 32].cp == cp) )
    {
        return &f->g[cp - 32];
    }
    for ( uint16_t i = 0; i < f->n; ++i )
    {
        if ( f->g[i].cp == cp ) { return &f->g[i]; }
    }
    return NULL;
}

static inline void blend(uint16_t *fb, int x, int y, uint16_t col, unsigned a4)
{
    if ( (0U == a4) || (x < 0) || (y < 0) || (x >= KK_W) || (y >= KK_H) ) { return; }
    uint16_t *const p = &fb[y * KK_W + x];
    *p = (15U == a4) ? col : kk_mix(*p, col, (int)((a4 * 256U) / 15U));
}

static void alpha_map(uint16_t *fb, const uint8_t *bits, uint32_t off, int w, int h, int x, int y, uint16_t col)
{
    if ( (x + w <= 0) || (y + h <= 0) || (x >= KK_W) || (y >= KK_H) ) { return; }
    uint32_t idx = off * 2U;   /* in nibbles */
    for ( int r = 0; r < h; ++r )
    {
        for ( int c = 0; c < w; ++c, ++idx )
        {
            uint8_t const b = bits[idx >> 1];
            unsigned const a = (idx & 1U) ? (b & 0x0FU) : (b >> 4);
            blend(fb, x + c, y + r, col, a);
        }
    }
}

int kf_width(const kf_font_t *f, const char *s)
{
    int w = 0;
    for ( ; *s; ++s )
    {
        const kf_glyph_t *const g = find(f, *s);
        w += g ? g->adv : (f->cap / 2);
    }
    return w;
}

int kf_draw(uint16_t *fb, const kf_font_t *f, int x, int baseline, const char *s, uint16_t col)
{
    int const x0 = x;
    if ( NULL == fb ) { return 0; }
    for ( ; *s; ++s )
    {
        const kf_glyph_t *const g = find(f, *s);
        if ( NULL == g )
        {
            x += f->cap / 2;
            continue;
        }
        if ( g->w && g->h ) { alpha_map(fb, f->bits, g->off, g->w, g->h, x + g->x0, baseline + g->y0, col); }
        x += g->adv;
    }
    return x - x0;
}

int kf_draw_top(uint16_t *fb, const kf_font_t *f, int x, int top, const char *s, uint16_t col)
{
    return kf_draw(fb, f, x, top + f->cap, s, col);
}

void kf_draw_c(uint16_t *fb, const kf_font_t *f, int cx, int top, const char *s, uint16_t col)
{
    (void)kf_draw(fb, f, cx - kf_width(f, s) / 2, top + f->cap, s, col);
}

void kf_icon(uint16_t *fb, const kf_icon_t *ic, int cx, int cy, uint16_t col)
{
    if ( NULL == fb ) { return; }
    alpha_map(fb, ic->bits, 0, ic->w, ic->h, cx - ic->w / 2, cy - ic->h / 2, col);
}

/* ---- kodekit's sizes ---- */

static const kf_font_t *for_scale(const char *s, int scale)
{
    if ( scale <= 1 ) { return &KF_CAPTION; }
    if ( 2 == scale ) { return &KF_BODY; }
    bool digits = true;
    for ( const char *p = s; *p; ++p )
    {
        if ( !strchr("0123456789:- ", *p) ) { digits = false; break; }
    }
    if ( digits && (scale >= 9) ) { return &KF_CLOCK; }
    if ( digits && (scale >= 5) ) { return &KF_FIGURE; }
    return &KF_TITLE;
}

int kf_compat_w(const char *s, int scale) { return kf_width(for_scale(s, scale), s); }

int kf_compat(uint16_t *fb, int x, int y, const char *s, int scale, uint16_t col)
{
    const kf_font_t *const f = for_scale(s, scale);
    /* The old 5x7 box was 7*scale high: keep the text's capitals centred on it. */
    int const mid = y + (7 * scale) / 2;
    return kf_draw(fb, f, x, mid + f->cap / 2, s, col);
}

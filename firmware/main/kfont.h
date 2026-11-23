/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kfont.h
 * @brief kodeOS's type and glyphs on the watch (see kos_assets.h for the sets).
 */
#ifndef KFONT_H_
#define KFONT_H_

#include <stdbool.h>
#include <stdint.h>

#include "kos_assets.h"

/** @brief Draw @p s with its baseline at @p baseline. @return the advance, in pixels. */
int kf_draw(uint16_t *fb, const kf_font_t *f, int x, int baseline, const char *s, uint16_t col);
/** @brief The same with the capitals' top at @p top. */
int kf_draw_top(uint16_t *fb, const kf_font_t *f, int x, int top, const char *s, uint16_t col);
/** @brief Centred on @p cx, capitals' top at @p top. */
void kf_draw_c(uint16_t *fb, const kf_font_t *f, int cx, int top, const char *s, uint16_t col);
int kf_width(const kf_font_t *f, const char *s);
/** @brief A glyph, centred on (cx, cy), in @p col. */
void kf_icon(uint16_t *fb, const kf_icon_t *ic, int cx, int cy, uint16_t col);

/* kodekit's kk_text() sizes on kodeOS's type. */
int kf_compat(uint16_t *fb, int x, int y, const char *s, int scale, uint16_t col);
int kf_compat_w(const char *s, int scale);

#endif /* KFONT_H_ */

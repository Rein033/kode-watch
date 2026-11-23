/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kody.h
 * @brief Kody on the watch's face: kodeOS's scenes, rendered ahead of time
 *        (tools/kodyframes.py) and played from the "kody" data partition.
 *        Kody's art: CC-BY-SA-4.0, kodediy.
 */
#ifndef KODY_H_
#define KODY_H_

#include <stdbool.h>
#include <stdint.h>

#define KODY_W 410
#define KODY_H 334

typedef enum { KODY_LOOP, KODY_NOTICE, KODY_PLUGGED, KODY_LOOKS, KODY_TIME, KODY_WAKE } kody_scene_t;

/** @brief Find the frames (the partition on the watch, a blob in memory elsewhere). */
bool kody_init(const uint8_t *blob);
bool kody_ready(void);
/** @brief Play @p s once, then go back to the loop (KODY_LOOP itself loops). */
void kody_play(kody_scene_t s);
/** @brief Bring Kody's picture up to @p now_ms and draw it at (0, y) into @p fb. */
void kody_draw(uint16_t *fb, int y, uint32_t now_ms);
/** @brief Pick the world for hour @p h (0..23), as kodeOS does; -1 is unknown. */
void kody_set_hour(int h);
/** @brief How many worlds the frames hold, and the one on the glass. */
int kody_worlds(void);
const char *kody_world(void);
/** @brief "ok", or what went wrong, for the Info page. */
const char *kody_status(void);

#endif /* KODY_H_ */

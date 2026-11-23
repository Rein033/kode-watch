/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sound.h
 * @brief The speaker (ES8311 + amplifier on GPIO46): short tones made here, played by their own task.
 */
#ifndef SOUND_H_
#define SOUND_H_

#include <stdbool.h>

typedef enum { SND_NOTE, SND_ALARM, SND_CLICK, SND_HELLO } sound_t;

void sound_init(void);
/** @brief Play @p s (queued, never blocks); nothing when sound is off. */
void sound_play(sound_t s);
void sound_set_on(bool on);
bool sound_on(void);

#endif /* SOUND_H_ */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file craft.h
 * @brief Craft: a Minecraft Java 1.21.8 server on the watch - bareiron (p2r3,
 *        GPL-3.0), the server KodeCraft is built on, run as a task on the OS's Wi-Fi.
 */
#ifndef CRAFT_H_
#define CRAFT_H_

#include <stdbool.h>

typedef enum { CRAFT_OFF, CRAFT_RUNNING, CRAFT_FAILED } craft_state_t;

/** @brief Start the server (once; it runs until the watch restarts). */
bool craft_start(void);
craft_state_t craft_state(void);
/** @brief Players connected now. */
int craft_players(void);

#endif /* CRAFT_H_ */

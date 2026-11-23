/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wifi_ui.h
 * @brief The watch's Wi-Fi screen (scan, pick a network, type the password).
 */
#ifndef WIFI_UI_H_
#define WIFI_UI_H_

#include <stdbool.h>

void wifi_ui_draw(void);
void wifi_ui_tap(int x, int y);
/** @brief True while the keyboard or the join view is open: swipes must not change screen. */
bool wifi_ui_modal(void);
/** @brief Back to the list (BOT). */
void wifi_ui_back(void);

#endif /* WIFI_UI_H_ */

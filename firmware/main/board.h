/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file board.h
 * @brief Kode-watch board layer for the Waveshare ESP32-S3-Touch-AMOLED-2.06:
 *        CO5300 panel over QSPI, FT3168 touch, AXP2101 PMU, PCF85063 RTC and
 *        the Kode Link over the USB serial port. kodekit (kk.c) sits on top.
 */
#ifndef BOARD_H_
#define BOARD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bring up I2C, the panel, touch, PMU, RTC, BOOT button and the USB link. */
void board_init(void);

/** @brief One of the two frame buffers (PSRAM, KK_W x KK_H RGB565, native byte order). */
uint16_t *board_fb(int i);

/** @brief Send a whole frame to the panel. Waits for the previous frame, then returns at once. */
void board_flush(const uint16_t *fb);

uint32_t board_ms(void);

/** @brief Buttons as KK_* bits: BOOT is TOP (held), a short PWR press is BOT (one frame). */
uint32_t board_buttons(void);

bool board_touch(int *x, int *y);

/** @brief Battery percentage from the AXP2101 gauge, -1 when there is no battery. */
int board_battery(void);
bool board_power_in(void);    /* USB (VBUS) present: in the dock or on a cable */
bool board_charging(void);
void board_brightness(int pct);
void board_panel(bool on);    /* off: the AMOLED sleeps (applied with the next flushed frame) */
/** @brief Turn the watch off (AXP2101); a press on PWR turns it on. */
void board_power_off(void);

/** @brief Local wall time (seconds, the clock read as if UTC) from the RTC; false when it lost time. */
bool board_rtc_get(int64_t *local_epoch);
void board_rtc_set(int64_t local_epoch);

/* The Kode Link, one text line per message. kk.c declares these weak. */
void kdotsim_link_send(const char *msg, size_t len);
int kdotsim_link_recv(char *buf, size_t cap);

#endif /* BOARD_H_ */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ble.h
 * @brief The iPhone over Bluetooth LE: notifications (ANCS), music (AMS), time (CTS).
 *
 * The watch advertises as "Kode Watch" and solicits ANCS, so iOS lists it under
 * Settings > Bluetooth. Pairing is a numeric comparison: the code shows on both
 * the watch and the iPhone, and the watch asks before it accepts.
 *
 * ble_recv() hands the OS lines in its own words:
 *   bnote <uid> <app>|<title>|<message>   a new notification
 *   bcall <uid> <caller>                  an incoming call
 *   bnrm <uid>                            the notification or call is gone
 *   btime <local-epoch>                   the iPhone's local time
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready;            /* the stack is up and advertising or connected */
    bool connected;
    bool secure;           /* encrypted and bonded */
    bool bonded_any;       /* a phone is remembered */
    bool ancs, ams, cts;   /* services found on the phone */
    char peer[32];         /* the phone's name, when read */
    bool numcmp_pending;   /* the watch asks: does this code match the iPhone? */
    bool passkey_show;     /* legacy pairing: type numcmp on the iPhone */
    uint32_t numcmp;
    int playing;           /* -1 unknown, 0 paused, 1 playing */
    char title[48];
    char artist[48];
} ble_status_t;

enum { BLE_MUSIC_PLAY = 0, BLE_MUSIC_PAUSE = 1, BLE_MUSIC_TOGGLE = 2, BLE_MUSIC_NEXT = 3,
       BLE_MUSIC_PREV = 4, BLE_MUSIC_VOL_UP = 5, BLE_MUSIC_VOL_DOWN = 6 };

void ble_init(void);
bool ble_recv(char *buf, int cap);
void ble_status(ble_status_t *st);
void ble_pair_answer(bool yes);
void ble_music(int cmd);
void ble_notif_action(uint32_t uid, bool positive);
void ble_forget(void);
/** @brief Bluetooth off until the next start (frees internal RAM, e.g. for an update's TLS). */
void ble_stop(void);

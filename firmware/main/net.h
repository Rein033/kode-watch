/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net.h
 * @brief Kode-watch network: Wi-Fi station and the WebSocket to the kode hub.
 */
#ifndef NET_H_
#define NET_H_

#include <stdbool.h>
#include <stddef.h>

/** @brief Called (from the WebSocket task) for every link message that comes in. */
typedef void (*net_rx_fn)(const char *msg, size_t len);

/** @brief Start Wi-Fi and the hub link with the settings in NVS (needs nvs_flash_init first). */
void net_init(net_rx_fn rx);

/** @brief Send one link message to the hub; false when the hub is not there. */
bool net_send(const char *msg, size_t len);
/** @brief A binary frame to the hub (the screen preview). */
bool net_send_bin(const void *data, size_t len);
void net_set_bin_rx(net_rx_fn fn);   /* whole binary frames from the hub (on the socket's task) */

bool net_hub_up(void);
/** @brief Wi-Fi power save off (true) for a live remote, back on (false). */
void net_low_latency(bool on);
void net_rest(bool resting);   /* resting: max modem sleep (the link answers a little later) */

/** @brief Handle a "cfg ..." line from USB; true when it was one. */
bool net_cfg_line(const char *line);

/* ---- for the Wi-Fi screen ---- */

typedef struct {
    char ssid[33];
    int rssi;
    bool open;
} net_ap_t;

typedef struct {
    bool wifi_up, hub_up, key_set;
    char ssid[33];
    char ip[16];
    char err[48];     /* the last problem, "" when there is none */
    char pair[8];     /* the pairing code the hub gave, "" when not pairing */
} net_status_t;

void net_scan_start(void);
bool net_scanning(void);
/** @brief Networks from the last scan, strongest first; 0 until a scan finished. */
int net_scan_results(net_ap_t *out, int max);
/** @brief Save and join this network (pass may be "" for an open one). */
void net_connect(const char *ssid, const char *pass);
void net_status(net_status_t *st);
/** @brief Is this network saved (the watch knows its password)? */
bool net_known(const char *ssid);
/** @brief Join a saved network with its saved password; false when it is not saved. */
bool net_join_known(const char *ssid);
/** @brief Forget a saved network. */
void net_forget(const char *ssid);

#endif /* NET_H_ */

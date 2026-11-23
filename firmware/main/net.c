/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net.c
 * @brief Kode-watch network: Wi-Fi station plus a WebSocket to the kode hub.
 *
 * Settings live in NVS namespace "kw". They are set on the watch itself (the
 * Wi-Fi screen: scan, pick, type the password) or over USB serial as
 * "cfg ssid <x>", "cfg pass <x>", "cfg key <x>", "cfg url <x>", then "cfg save"
 * (which reconnects). "cfg show" prints what is set, without the secrets.
 * Link messages are WebSocket text frames, one message per frame.
 */
#include "esp_attr.h"
#include "net.h"

#include "esp_timer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "net";
static net_rx_fn s_bin_rx;
EXT_RAM_BSS_ATTR static uint8_t s_bin[8192];
static size_t s_bin_n;
static bool s_bin_on;

#define DEFAULT_URL "wss://hub.example.com/ws-dev"
#define MAX_APS 16

static char s_ssid[33], s_pass[65], s_key[64], s_url[128];

/* saved networks, last used first; the watch roams to the strongest one it can see */
#define KNOWN_MAX 8
typedef struct { char ssid[33]; char pass[65]; } known_t;
static known_t s_known[KNOWN_MAX];
static int s_nknown;
static int s_fail;            /* failed joins since the last success */
static int64_t s_roam_at;     /* the last roam scan (us) */
static bool s_roam;           /* the running scan is a roam scan */
static char s_hdr[96];
static char s_ip[16];
static char s_err[48];
/* Pairing: with no key the watch opens the hub's socket as "Bearer pair"; the
 * hub answers "pair <code>", and once the code is approved on the hub's
 * /oud/pair page (behind the login) it sends "key <key>". */
static bool s_pairing;
static char s_pair[8];
static volatile bool s_rekey;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_wifi_up, s_scanning, s_scan_done;
static net_rx_fn s_rx;
static net_ap_t s_aps[MAX_APS];
static int s_naps;

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
}

static void load_str(nvs_handle_t h, const char *k, char *dst, size_t cap, const char *def)
{
    size_t n = cap;
    if ( ESP_OK != nvs_get_str(h, k, dst, &n) ) { (void)snprintf(dst, cap, "%s", def); }
}

static void load(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = s_key[0] = '\0';
    (void)snprintf(s_url, sizeof(s_url), "%s", DEFAULT_URL);
    if ( ESP_OK != nvs_open("kw", NVS_READONLY, &h) ) { return; }
    load_str(h, "ssid", s_ssid, sizeof(s_ssid), "");
    load_str(h, "pass", s_pass, sizeof(s_pass), "");
    load_str(h, "key", s_key, sizeof(s_key), "");
    load_str(h, "url", s_url, sizeof(s_url), DEFAULT_URL);
    size_t kn = sizeof(s_known);
    if ( ESP_OK == nvs_get_blob(h, "known", s_known, &kn) ) { s_nknown = (int)(kn / sizeof(known_t)); }
    nvs_close(h);
    if ( (0 == s_nknown) && s_ssid[0] )   /* from before 0.24: the one network becomes the first saved one */
    {
        (void)snprintf(s_known[0].ssid, sizeof(s_known[0].ssid), "%s", s_ssid);
        (void)snprintf(s_known[0].pass, sizeof(s_known[0].pass), "%s", s_pass);
        s_nknown = 1;
    }
}

static void save(void)
{
    nvs_handle_t h;
    if ( ESP_OK != nvs_open("kw", NVS_READWRITE, &h) ) { return; }
    (void)nvs_set_str(h, "ssid", s_ssid);
    (void)nvs_set_str(h, "pass", s_pass);
    (void)nvs_set_str(h, "key", s_key);
    (void)nvs_set_str(h, "url", s_url);
    (void)nvs_set_blob(h, "known", s_known, sizeof(known_t) * (size_t)s_nknown);
    (void)nvs_commit(h);
    nvs_close(h);
}

static int known_find(const char *ssid)
{
    for ( int i = 0; i < s_nknown; ++i ) { if ( 0 == strcmp(s_known[i].ssid, ssid) ) { return i; } }
    return -1;
}

/* this network (and password) to the front of the saved list; true when the list changed */
static bool known_put(const char *ssid, const char *pass)
{
    if ( '\0' == ssid[0] ) { return false; }
    int i = known_find(ssid);
    if ( (0 == i) && (0 == strcmp(s_known[0].pass, pass)) ) { return false; }
    if ( i < 0 ) { i = (s_nknown < KNOWN_MAX) ? s_nknown++ : (KNOWN_MAX - 1); }   /* full: the oldest goes */
    for ( ; i > 0; --i ) { s_known[i] = s_known[i - 1]; }
    (void)snprintf(s_known[0].ssid, sizeof(s_known[0].ssid), "%s", ssid);
    (void)snprintf(s_known[0].pass, sizeof(s_known[0].pass), "%s", pass);
    return true;
}

bool net_known(const char *ssid) { return known_find(ssid) >= 0; }

/* ---- WebSocket ---- */

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t const *const e = data;
    switch ( id )
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "hub connected%s", s_pairing ? ", pairing" : "");
        s_err[0] = '\0';
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "hub disconnected");
        if ( s_rx ) { s_rx("hub peer 0", 10); }
        break;
    case WEBSOCKET_EVENT_ERROR:
        if ( e->error_handle.esp_ws_handshake_status_code )
        {
            set_err("HUB HTTP %d", e->error_handle.esp_ws_handshake_status_code);
            if ( (403 == e->error_handle.esp_ws_handshake_status_code) && !s_pairing )
            {
                /* The hub no longer knows this key: forget it and pair again. */
                s_key[0] = '\0';
                save();
                s_rekey = true;
            }
        }
        else if ( e->error_handle.esp_tls_last_esp_err )
        {
            set_err("HUB %s", esp_err_to_name(e->error_handle.esp_tls_last_esp_err));
        }
        else if ( e->error_handle.esp_transport_sock_errno )
        {
            set_err("HUB SOCKET %d", e->error_handle.esp_transport_sock_errno);
        }
        else
        {
            set_err("HUB ERROR %d", (int)e->error_handle.error_type);
        }
        ESP_LOGW(TAG, "%s", s_err);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* Text frames only, and only whole ones (link messages are small). */
        if ( (0x02 == e->op_code) && (0 == e->payload_offset) && !s_bin_on ) { ESP_LOGD(TAG, "bin %d/%d fin %d", e->data_len, e->payload_len, e->fin); }
        if ( (0x01 == e->op_code) && (0 == e->payload_offset) && (e->data_len == e->payload_len) && s_pairing )
        {
            char m[80];
            int const n = (e->data_len < (int)sizeof(m) - 1) ? e->data_len : (int)sizeof(m) - 1;
            memcpy(m, e->data_ptr, (size_t)n);
            m[n] = '\0';
            if ( (0 == strncmp(m, "pair ", 5)) && (6 == strlen(m + 5)) )
            {
                (void)snprintf(s_pair, sizeof(s_pair), "%s", m + 5);
                ESP_LOGI(TAG, "pairing code %s", s_pair);
                if ( s_rx )
                {
                    char note[96];
                    int const k = snprintf(note, sizeof(note), "notify Pair code %.3s %.3s: enter it at hub.example.com/pair", s_pair, s_pair + 3);
                    s_rx(note, (size_t)k);
                }
            }
            else if ( (0 == strncmp(m, "key ", 4)) && (strlen(m + 4) >= 16) && (strlen(m + 4) < sizeof(s_key)) )
            {
                (void)snprintf(s_key, sizeof(s_key), "%s", m + 4);
                save();
                s_pair[0] = '\0';
                ESP_LOGI(TAG, "paired: key saved");
                s_rekey = true;
            }
        }
        else if ( (0x01 == e->op_code) && (0 == e->payload_offset) && (e->data_len == e->payload_len) && s_rx )
        {
            s_rx(e->data_ptr, (size_t)e->data_len);
        }
        else if ( ((0x02 == e->op_code) || ((0x00 == e->op_code) && s_bin_on)) && s_bin_rx && !s_pairing )
        {
            /* A binary message may come in pieces (a frame read in parts, or
             * continuation frames): put it together, hand it on when whole. */
            if ( (0x02 == e->op_code) && (0 == e->payload_offset) ) { s_bin_n = 0; s_bin_on = true; }
            if ( s_bin_on && (e->data_len > 0) )
            {
                if ( s_bin_n + (size_t)e->data_len > sizeof(s_bin) ) { s_bin_on = false; ESP_LOGW(TAG, "binary message too big"); break; }
                memcpy(s_bin + s_bin_n, e->data_ptr, (size_t)e->data_len);
                s_bin_n += (size_t)e->data_len;
            }
            if ( s_bin_on && e->fin && ((e->payload_offset + e->data_len) >= e->payload_len) )
            {
                s_bin_on = false;
                s_bin_rx((const char *)s_bin, s_bin_n);
            }
        }
        break;
    default:
        break;
    }
}

static void ws_start(void)
{
    if ( s_ws ) { return; }
    s_pairing = ('\0' == s_key[0]);
    (void)snprintf(s_hdr, sizeof(s_hdr), "Authorization: Bearer %s\r\n", s_pairing ? "pair" : s_key);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = s_hdr,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .buffer_size = 4096,   /* a file chunk (3 KB) arrives whole */
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if ( NULL == s_ws )
    {
        set_err("HUB START ERROR");
        return;
    }
    (void)esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    (void)esp_websocket_client_start(s_ws);
}

static void ws_stop(void)
{
    if ( NULL == s_ws ) { return; }
    (void)esp_websocket_client_stop(s_ws);
    (void)esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

bool net_send(const char *msg, size_t len)
{
    if ( (NULL == s_ws) || !esp_websocket_client_is_connected(s_ws) ) { return false; }
    return esp_websocket_client_send_text(s_ws, msg, (int)len, pdMS_TO_TICKS(200)) >= 0;
}

void net_set_bin_rx(net_rx_fn fn) { s_bin_rx = fn; }

bool net_send_bin(const void *data, size_t len)
{
    if ( (NULL == s_ws) || s_pairing || !esp_websocket_client_is_connected(s_ws) ) { return false; }
    return esp_websocket_client_send_bin(s_ws, (const char *)data, (int)len, pdMS_TO_TICKS(1000)) >= 0;
}

bool net_hub_up(void) { return s_ws && !s_pairing && esp_websocket_client_is_connected(s_ws); }

void net_rest(bool resting)
{
    (void)esp_wifi_set_ps(resting ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
}

void net_low_latency(bool on)
{
    /* Modem sleep holds frames until the AP's next beacon: up to a few hundred
     * ms, longer than the RC car's 300 ms failsafe. */
    esp_err_t const e = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "power save %s: %s", on ? "off" : "min", esp_err_to_name(e));
}

/* A new key, or a key the hub refused: the socket starts again, off the
 * client's own task (it cannot destroy itself from its event handler). */
static void rekey_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        vTaskDelay(pdMS_TO_TICKS(500));
        if ( s_rekey )
        {
            s_rekey = false;
            ws_stop();
            if ( s_wifi_up ) { ws_start(); }
        }
    }
}

/* ---- Wi-Fi ---- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if ( (WIFI_EVENT == base) && (WIFI_EVENT_STA_START == id) )
    {
        if ( s_ssid[0] ) { (void)esp_wifi_connect(); }
    }
    else if ( (WIFI_EVENT == base) && (WIFI_EVENT_STA_DISCONNECTED == id) )
    {
        wifi_event_sta_disconnected_t const *const d = data;
        bool const was_up = s_wifi_up;
        s_wifi_up = false;
        s_ip[0] = '\0';
        if ( (WIFI_REASON_STA_LEAVING == d->reason) || (WIFI_REASON_ASSOC_LEAVE == d->reason) )
        {
            /* our own disconnect (a new network or a scan): no error; wifi_apply / the scan end connect again */
            ESP_LOGI(TAG, "left (%d)", d->reason);
            if ( !s_scanning && s_ssid[0] ) { (void)esp_wifi_connect(); }
            return;
        }
        switch ( d->reason )
        {
        case WIFI_REASON_NO_AP_FOUND: set_err("NETWORK NOT FOUND"); break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: set_err("WRONG PASSWORD?"); break;
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            set_err(s_pass[0] ? "SECURITY MISMATCH (%d)" : "NO PASSWORD (%d)", d->reason);
            break;
        default: if ( !was_up || !s_err[0] ) { set_err("WIFI REASON %d", d->reason); } break;
        }
        ++s_fail;
        int64_t const now = esp_timer_get_time();
        if ( !s_scanning && !was_up && (s_nknown > 1) && (s_fail >= 2) && ((now - s_roam_at) > 15000000LL) )
        {
            s_roam_at = now;
            s_roam = true;
            s_scanning = true;
            s_scan_done = false;
            if ( ESP_OK == esp_wifi_scan_start(NULL, false) ) { ESP_LOGI(TAG, "roam scan"); return; }
            s_roam = false;
            s_scanning = false;
            s_scan_done = true;
        }
        if ( !s_scanning && s_ssid[0] ) { (void)esp_wifi_connect(); }
    }
    else if ( (WIFI_EVENT == base) && (WIFI_EVENT_SCAN_DONE == id) )
    {
        s_naps = 0;
        uint16_t n = 0;
        (void)esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *const recs = (n > 0) ? calloc(n, sizeof(*recs)) : NULL;
        int best = -1, best_rssi = -1000;
        if ( recs && (ESP_OK == esp_wifi_scan_get_ap_records(&n, recs)) )
        {
            for ( int i = 0; i < n; ++i )   /* the strongest saved network in range */
            {
                int const k = known_find((const char *)recs[i].ssid);
                if ( (k >= 0) && (recs[i].rssi > best_rssi) ) { best = k; best_rssi = recs[i].rssi; }
            }
            for ( int i = 0; i < n; ++i )
            {
                const char *const ss = (const char *)recs[i].ssid;
                if ( '\0' == ss[0] ) { continue; }
                int j = 0;
                while ( (j < s_naps) && strcmp(s_aps[j].ssid, ss) ) { ++j; }
                if ( j < s_naps )
                {
                    if ( recs[i].rssi > s_aps[j].rssi ) { s_aps[j].rssi = recs[i].rssi; }
                    continue;
                }
                if ( s_naps == MAX_APS ) { continue; }
                (void)snprintf(s_aps[s_naps].ssid, sizeof(s_aps[0].ssid), "%s", ss);
                s_aps[s_naps].rssi = recs[i].rssi;
                s_aps[s_naps].open = (WIFI_AUTH_OPEN == recs[i].authmode);
                ++s_naps;
            }
            /* strongest first */
            for ( int a = 1; a < s_naps; ++a )
            {
                net_ap_t const t = s_aps[a];
                int b = a - 1;
                while ( (b >= 0) && (s_aps[b].rssi < t.rssi) ) { s_aps[b + 1] = s_aps[b]; --b; }
                s_aps[b + 1] = t;
            }
        }
        else
        {
            (void)esp_wifi_clear_ap_list();
        }
        free(recs);
        s_scanning = false;
        s_scan_done = true;
        bool const roam = s_roam;
        s_roam = false;
        if ( !s_wifi_up && (best >= 0) && (roam || (s_fail >= 2)) && strcmp(s_known[best].ssid, s_ssid) )
        {
            ESP_LOGI(TAG, "roam to %s (%d dBm)", s_known[best].ssid, best_rssi);
            (void)snprintf(s_ssid, sizeof(s_ssid), "%s", s_known[best].ssid);
            (void)snprintf(s_pass, sizeof(s_pass), "%s", s_known[best].pass);
            s_err[0] = '\0';
            wifi_config_t wc = { 0 };
            (void)snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", s_ssid);
            (void)snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", s_pass);
            wc.sta.threshold.authmode = ('\0' == s_pass[0]) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
            wc.sta.pmf_cfg.capable = true;
            (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        if ( s_ssid[0] && !s_wifi_up ) { (void)esp_wifi_connect(); }
    }
    else if ( (IP_EVENT == base) && (IP_EVENT_STA_GOT_IP == id) )
    {
        ip_event_got_ip_t const *const g = data;
        (void)snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&g->ip_info.ip));
        s_wifi_up = true;
        s_err[0] = '\0';
        s_fail = 0;
        if ( known_put(s_ssid, s_pass) ) { save(); }   /* joined: saved, and first in the list */
        ESP_LOGI(TAG, "wifi up %s", s_ip);
        ws_start();
    }
}

static void wifi_apply(void)
{
    ws_stop();
    (void)esp_wifi_disconnect();
    s_wifi_up = false;
    s_err[0] = '\0';
    if ( '\0' == s_ssid[0] ) { return; }
    ESP_LOGI(TAG, "join %s (password %u chars)", s_ssid, (unsigned)strlen(s_pass));
    wifi_config_t wc = { 0 };
    (void)snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", s_ssid);
    (void)snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", s_pass);
    wc.sta.threshold.authmode = ('\0' == s_pass[0]) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
    (void)esp_wifi_connect();
}

void net_init(net_rx_fn rx)
{
    s_rx = rx;
    load();
    xTaskCreate(rekey_task, "rekey", 4096, NULL, 3, NULL);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();
    wifi_init_config_t const wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());     /* always on, so the Wi-Fi screen can scan */
    wifi_apply();
}

/* ---- for the Wi-Fi screen ---- */

void net_scan_start(void)
{
    if ( s_scanning ) { return; }
    s_scan_done = false;
    s_scanning = true;
    if ( !s_wifi_up ) { (void)esp_wifi_disconnect(); }   /* stop a connect loop, it blocks the scan */
    if ( ESP_OK != esp_wifi_scan_start(NULL, false) )
    {
        s_scanning = false;
        s_scan_done = true;
        set_err("SCAN FAILED");
        if ( s_ssid[0] && !s_wifi_up ) { (void)esp_wifi_connect(); }
    }
}

bool net_scanning(void) { return s_scanning; }

int net_scan_results(net_ap_t *out, int max)
{
    if ( !s_scan_done ) { return 0; }
    int const n = (s_naps < max) ? s_naps : max;
    memcpy(out, s_aps, sizeof(net_ap_t) * (size_t)n);
    return n;
}

bool net_join_known(const char *ssid)
{
    int const k = known_find(ssid);
    if ( k < 0 ) { return false; }
    char p[65];
    (void)snprintf(p, sizeof(p), "%s", s_known[k].pass);
    net_connect(ssid, p);
    return true;
}

void net_forget(const char *ssid)
{
    int const k = known_find(ssid);
    if ( k < 0 ) { return; }
    for ( int i = k; i < s_nknown - 1; ++i ) { s_known[i] = s_known[i + 1]; }
    --s_nknown;
    save();
}

void net_connect(const char *ssid, const char *pass)
{
    s_fail = 0;
    (void)snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    (void)snprintf(s_pass, sizeof(s_pass), "%s", pass ? pass : "");
    save();
    wifi_apply();
}

void net_status(net_status_t *st)
{
    st->wifi_up = s_wifi_up;
    st->hub_up = net_hub_up();
    st->key_set = ('\0' != s_key[0]);
    (void)snprintf(st->ssid, sizeof(st->ssid), "%s", s_ssid);
    (void)snprintf(st->ip, sizeof(st->ip), "%s", s_ip);
    (void)snprintf(st->err, sizeof(st->err), "%s", s_err);
    (void)snprintf(st->pair, sizeof(st->pair), "%s", s_pairing ? s_pair : "");
}

/* ---- settings over USB ---- */

bool net_cfg_line(const char *line)
{
    if ( 0 != strncmp(line, "cfg ", 4) ) { return false; }
    const char *const a = line + 4;
    if ( 0 == strncmp(a, "ssid ", 5) ) { (void)snprintf(s_ssid, sizeof(s_ssid), "%s", a + 5); }
    else if ( 0 == strncmp(a, "pass ", 5) ) { (void)snprintf(s_pass, sizeof(s_pass), "%s", a + 5); }
    else if ( 0 == strcmp(a, "pass") ) { s_pass[0] = '\0'; }
    else if ( 0 == strncmp(a, "key ", 4) ) { (void)snprintf(s_key, sizeof(s_key), "%s", a + 4); }
    else if ( 0 == strncmp(a, "url ", 4) ) { (void)snprintf(s_url, sizeof(s_url), "%s", a + 4); }
    else if ( 0 == strcmp(a, "list") )
    {
        for ( int i = 0; i < s_nknown; ++i ) { printf("cfg known %d %s\n", i, s_known[i].ssid); }
    }
    else if ( 0 == strncmp(a, "forget ", 7) ) { net_forget(a + 7); printf("cfg forgot\n"); }
    else if ( 0 == strcmp(a, "save") )
    {
        (void)known_put(s_ssid, s_pass);
        save();
        wifi_apply();
        printf("cfg saved\n");
    }
    else if ( 0 == strcmp(a, "show") )
    {
        char pl[16];
        (void)snprintf(pl, sizeof(pl), "(%u chars)", (unsigned)strlen(s_pass));
        printf("cfg ssid=%s pass=%s key=%s url=%s wifi=%d ip=%s hub=%d err=%s\n", s_ssid,
               s_pass[0] ? pl : "(none)", s_key[0] ? "(set)" : "(none)", s_url, s_wifi_up, s_ip, net_hub_up(),
               s_err);
    }
    return true;
}

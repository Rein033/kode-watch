/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file wifi_ui.c
 * @brief The watch's Wi-Fi screen: status, a scan list, and an on-screen
 *        keyboard for the password. Drawn with kodekit, driven by taps.
 */
#include "wifi_ui.h"

#include <stdio.h>
#include <string.h>

#include "kk.h"
#include "net.h"

#define CX (KK_W / 2)

enum { LIST, KEYB, JOIN };

static int s_mode = LIST;
static net_ap_t s_aps[8];
static int s_naps;
static bool s_scanned_once;
static char s_sel[33];
static char s_pw[65];
static int s_layer;            /* 0 lower, 1 upper, 2 symbols */
static bool s_caps;            /* upper stays on (AA tapped twice) */
static uint32_t s_shift_at;
static uint32_t s_join_at;
static uint32_t s_short_at;     /* OK with fewer than 8 characters */

/* The key that was hit last, flashed for a moment so a miss is easy to see. */
static int s_hit_x, s_hit_y, s_hit_w;
static char s_hit_lbl[8];
static bool s_hit_char;
static uint32_t s_hit_at;
#define HIT_MS 300U

/* ---- keyboard layout ---- */

#define KEY_W 31
#define KEY_H 44
#define KEY_GAP 2
#define KB_X0 (CX - (10 * (KEY_W + KEY_GAP) - KEY_GAP) / 2)
#define KB_Y0 178

static const char *const ROWS[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" },
    { "1234567890", "!@#$%^&*()", "-_=+[]{};:", "'\",.<>/?\\|" },
};

/* The bottom row: wide keys. */
enum { K_SHIFT, K_SYM, K_SPACE, K_DEL, K_OK, K_N };
static const char *const BOT_LBL[K_N] = { "AA", "#+", "SPACE", "<-", "OK" };
static const int BOT_W[K_N] = { 46, 46, 118, 46, 64 };

static int row_x0(const char *row)
{
    int const n = (int)strlen(row);
    return CX - (n * (KEY_W + KEY_GAP) - KEY_GAP) / 2;
}

static void draw_key(int x, int y, int w, const char *lbl, uint16_t bg)
{
    kk_round(x, y, w, KEY_H, 8, bg);
    kk_text(x + (w - kk_text_w(lbl, 2)) / 2, y + (KEY_H - 14) / 2, lbl, 2, KK_WHITE);
}

static void draw_keyboard(void)
{
    kk_text_c(70, "PASSWORD FOR", 1, KK_GREY);
    char t[24];
    (void)snprintf(t, sizeof(t), "%.18s", s_sel);
    kk_text_c(86, t, 2, KK_CYAN);

    /* The field: the last 20 characters, with a cursor. */
    kk_round(44, 116, KK_W - 88, 46, 10, ((kk_ms() - s_short_at) < 1500U) ? KK_RGB(90, 20, 26) : KK_RGB(22, 26, 34));
    {
        char cnt[24];
        if ( (kk_ms() - s_short_at) < 1500U ) { (void)snprintf(cnt, sizeof(cnt), "MIN. 8 CHARACTERS"); }
        else { (void)snprintf(cnt, sizeof(cnt), "%u CHARS", (unsigned)strlen(s_pw)); }
        kk_text(KK_W - 44 - kk_text_w(cnt, 1) - 8, 106, cnt, 1, KK_GREY);
    }
    int const n = (int)strlen(s_pw);
    const char *const tail = (n > 20) ? (s_pw + n - 20) : s_pw;
    int const w = kk_text(58, 132, tail, 2, KK_WHITE);
    if ( ((kk_ms() / 500U) % 2U) == 0U ) { kk_rect(58 + w + 2, 130, 3, 18, KK_CYAN); }

    for ( int r = 0; r < 4; ++r )
    {
        const char *const row = ROWS[s_layer][r];
        int x = row_x0(row);
        int const y = KB_Y0 + r * (KEY_H + 6);
        for ( const char *p = row; *p; ++p )
        {
            char const lbl[2] = { *p, '\0' };
            draw_key(x, y, KEY_W, lbl, KK_RGB(40, 44, 54));
            x += KEY_W + KEY_GAP;
        }
    }
    int x = CX - (46 + 46 + 118 + 46 + 64 + 4 * 4) / 2;
    int const y = KB_Y0 + 4 * (KEY_H + 6);
    for ( int k = 0; k < K_N; ++k )
    {
        uint16_t bg = KK_RGB(28, 32, 42);
        if ( (K_SHIFT == k) && (1 == s_layer) ) { bg = s_caps ? KK_ORANGE : KK_BLUE; }
        if ( (K_SYM == k) && (2 == s_layer) ) { bg = KK_BLUE; }
        if ( K_OK == k ) { bg = KK_GREEN; }
        draw_key(x, y, BOT_W[k], BOT_LBL[k], bg);
        if ( (K_SHIFT == k) && s_caps ) { kk_rect(x + 10, y + KEY_H - 8, BOT_W[k] - 20, 3, KK_WHITE); }
        x += BOT_W[k] + 4;
    }
    if ( net_known(s_sel) )
    {
        kk_round(CX - 60, KB_Y0 + 5 * (KEY_H + 6) - 4, 120, 30, 10, KK_RGB(90, 20, 26));
        kk_text_c(KB_Y0 + 5 * (KEY_H + 6) + 5, "FORGET", 1, KK_WHITE);
    }
    else { kk_text_c(KB_Y0 + 5 * (KEY_H + 6) + 4, s_caps ? "CAPS LOCK  BOT: BACK" : "AA 2X: LOCK  BOT: BACK", 1, KK_GREY); }

    /* Flash the key just hit; a letter also gets a big preview above it. */
    if ( (kk_ms() - s_hit_at) < HIT_MS )
    {
        draw_key(s_hit_x, s_hit_y, s_hit_w, s_hit_lbl, KK_CYAN);
        if ( s_hit_char )
        {
            int const px = s_hit_x + s_hit_w / 2 - 26;
            kk_round(px, s_hit_y - 62, 52, 56, 12, KK_CYAN);
            kk_text(px + 11, s_hit_y - 52, s_hit_lbl, 5, KK_BLACK);
        }
    }
}

static void mark_hit(int x, int y, int w, const char *lbl, bool is_char)
{
    s_hit_x = x;
    s_hit_y = y;
    s_hit_w = w;
    (void)snprintf(s_hit_lbl, sizeof(s_hit_lbl), "%s", lbl);
    s_hit_char = is_char;
    s_hit_at = kk_ms();
}

static void type_char(char c)
{
    size_t const n = strlen(s_pw);
    if ( n < sizeof(s_pw) - 1 )
    {
        s_pw[n] = c;
        s_pw[n + 1] = '\0';
    }
    if ( (1 == s_layer) && !s_caps ) { s_layer = 0; }   /* one tap on AA is for one letter */
}

static void start_join(const char *pass)
{
    net_connect(s_sel, pass);
    s_join_at = kk_ms();
    s_mode = JOIN;
}

static void tap_keyboard(int tx, int ty)
{
    for ( int r = 0; r < 4; ++r )
    {
        int const y = KB_Y0 + r * (KEY_H + 6);
        if ( (ty < y) || (ty >= y + KEY_H + 6) ) { continue; }
        const char *const row = ROWS[s_layer][r];
        int const x0 = row_x0(row);
        int const i = (tx - x0) / (KEY_W + KEY_GAP);
        if ( (tx >= x0) && (i >= 0) && (i < (int)strlen(row)) )
        {
            char const lbl[2] = { row[i], '\0' };
            mark_hit(x0 + i * (KEY_W + KEY_GAP), y, KEY_W, lbl, true);
            type_char(row[i]);
        }
        return;
    }
    int const y = KB_Y0 + 4 * (KEY_H + 6);
    if ( (ty < y) || (ty >= y + KEY_H + 10) ) { return; }
    int x = CX - (46 + 46 + 118 + 46 + 64 + 4 * 4) / 2;
    for ( int k = 0; k < K_N; ++k )
    {
        if ( (tx >= x) && (tx < x + BOT_W[k] + 4) )
        {
            size_t const n = strlen(s_pw);
            uint32_t const now = kk_ms();
            mark_hit(x, y, BOT_W[k], BOT_LBL[k], false);
            switch ( k )
            {
            case K_SHIFT:
                if ( (1 == s_layer) && !s_caps && ((now - s_shift_at) < 500U) ) { s_caps = true; }
                else if ( 1 == s_layer ) { s_layer = 0; s_caps = false; }
                else { s_layer = 1; s_caps = false; }
                s_shift_at = now;
                break;
            case K_SYM: s_layer = (2 == s_layer) ? 0 : 2; s_caps = false; break;
            case K_SPACE: type_char(' '); break;
            case K_DEL: if ( n ) { s_pw[n - 1] = '\0'; } break;
            default:
                if ( strlen(s_pw) < 8 ) { s_short_at = now; }   /* WPA2 needs 8..63 */
                else { start_join(s_pw); }
                break;
            }
            kk_tone(700, 10);
            return;
        }
        x += BOT_W[k] + 4;
    }
}

/* ---- status and list ---- */

static void draw_status(int y)
{
    net_status_t st;
    net_status(&st);
    char t[40];
    if ( st.wifi_up ) { (void)snprintf(t, sizeof(t), "WIFI: %.16s", st.ssid); }
    else if ( st.ssid[0] ) { (void)snprintf(t, sizeof(t), "WIFI: TRY %.12s", st.ssid); }
    else { (void)snprintf(t, sizeof(t), "WIFI: NOT SET UP"); }
    kk_text_c(y, t, 2, st.wifi_up ? KK_GREEN : KK_YELLOW);
    if ( st.hub_up ) { (void)snprintf(t, sizeof(t), "HUB: CONNECTED"); }
    else if ( st.pair[0] ) { (void)snprintf(t, sizeof(t), "HUB: CODE %s", st.pair); }
    else if ( !st.key_set ) { (void)snprintf(t, sizeof(t), "HUB: PAIRING"); }
    else { (void)snprintf(t, sizeof(t), "HUB: NOT CONNECTED"); }
    kk_text_c(y + 22, t, 2, st.hub_up ? KK_GREEN : KK_GREY);
    if ( st.err[0] ) { kk_text_c(y + 46, st.err, 1, KK_RED); }
    else if ( st.ip[0] ) { kk_text_c(y + 46, st.ip, 1, KK_GREY); }
}

#define LIST_Y 172
#define ROW_H 40
#define ROWS_SHOWN 5
#define SCAN_Y 382

static void draw_list(void)
{
    kk_text_c(70, "WIFI", 3, KK_BLUE);
    draw_status(104);
    if ( !s_scanned_once && !net_scanning() )
    {
        s_scanned_once = true;
        net_scan_start();
    }
    if ( !net_scanning() ) { s_naps = net_scan_results(s_aps, 8); }
    net_status_t st;
    net_status(&st);
    for ( int i = 0; (i < s_naps) && (i < ROWS_SHOWN); ++i )
    {
        int const y = LIST_Y + i * ROW_H;
        bool const cur = (0 == strcmp(s_aps[i].ssid, st.ssid));
        kk_round(48, y, KK_W - 96, ROW_H - 4, 10, cur ? KK_RGB(10, 50, 70) : KK_RGB(22, 26, 34));
        char t[20];
        (void)snprintf(t, sizeof(t), "%.15s", s_aps[i].ssid);
        kk_text(60, y + 11, t, 2, KK_WHITE);
        int const bars = (s_aps[i].rssi > -55) ? 4 : (s_aps[i].rssi > -67) ? 3 : (s_aps[i].rssi > -78) ? 2 : 1;
        for ( int b = 0; b < 4; ++b )
        {
            kk_rect(KK_W - 92 + b * 7, y + 26 - b * 5, 5, 4 + b * 5, (b < bars) ? KK_WHITE : KK_RGB(60, 64, 72));
        }
        if ( net_known(s_aps[i].ssid) ) { kk_text(KK_W - 122, y + 11, "OK", 1, KK_GREEN); }
        else if ( !s_aps[i].open ) { kk_text(KK_W - 118, y + 11, "*", 2, KK_GREY); }
    }
    if ( net_scanning() ) { kk_text_c(LIST_Y + 60, "SCANNING...", 2, KK_GREY); }
    else if ( 0 == s_naps ) { kk_text_c(LIST_Y + 60, "NO NETWORKS", 2, KK_GREY); }
    kk_round(CX - 80, SCAN_Y, 160, 40, 12, KK_RGB(28, 32, 42));
    kk_text_c(SCAN_Y + 13, "SCAN AGAIN", 1, KK_WHITE);
}

static void draw_join(void)
{
    kk_text_c(70, "WIFI", 3, KK_BLUE);
    char t[24];
    (void)snprintf(t, sizeof(t), "%.18s", s_sel);
    kk_text_c(150, t, 2, KK_CYAN);
    net_status_t st;
    net_status(&st);
    if ( st.wifi_up )
    {
        kk_text_c(200, "CONNECTED", 3, KK_GREEN);
        draw_status(260);
        kk_text_c(400, "TAP: DONE", 2, KK_GREY);
    }
    else if ( st.err[0] && ((kk_ms() - s_join_at) > 3000U) )
    {
        kk_text_c(200, "FAILED", 3, KK_RED);
        kk_text_c(250, st.err, 2, KK_RED);
        kk_text_c(400, "TAP: RETRY", 2, KK_GREY);
    }
    else
    {
        static const char *const SPIN[4] = { "CONNECTING", "CONNECTING.", "CONNECTING..", "CONNECTING..." };
        kk_text_c(220, SPIN[(kk_ms() / 400U) % 4U], 2, KK_WHITE);
    }
}

/* ---- the screen ---- */

bool wifi_ui_modal(void) { return LIST != s_mode; }

void wifi_ui_draw(void)
{
    switch ( s_mode )
    {
    case KEYB: draw_keyboard(); break;
    case JOIN: draw_join(); break;
    default: draw_list(); break;
    }
}

void wifi_ui_back(void)
{
    s_mode = LIST;
    s_pw[0] = '\0';
}

void wifi_ui_tap(int x, int y)
{
    if ( KEYB == s_mode )
    {
        int const fy = KB_Y0 + 5 * (KEY_H + 6) - 4;
        if ( net_known(s_sel) && (y >= fy) && (y < fy + 34) && (x >= CX - 60) && (x < CX + 60) )
        {
            net_forget(s_sel);
            kk_tone(400, 40);
            wifi_ui_back();
            return;
        }
        tap_keyboard(x, y);
        return;
    }
    if ( JOIN == s_mode )
    {
        net_status_t st;
        net_status(&st);
        if ( st.wifi_up ) { wifi_ui_back(); }
        else if ( st.err[0] ) { s_mode = KEYB; }    /* try the password again */
        return;
    }
    if ( (y >= SCAN_Y) && (y < SCAN_Y + 40) && (x >= CX - 80) && (x < CX + 80) )
    {
        net_scan_start();
        kk_tone(600, 15);
        return;
    }
    int const i = (y - LIST_Y) / ROW_H;
    if ( (y >= LIST_Y) && (i >= 0) && (i < s_naps) && (i < ROWS_SHOWN) )
    {
        (void)snprintf(s_sel, sizeof(s_sel), "%s", s_aps[i].ssid);
        s_pw[0] = '\0';
        s_layer = 0;
        s_caps = false;
        if ( net_join_known(s_sel) ) { s_join_at = kk_ms(); s_mode = JOIN; }
        else if ( s_aps[i].open ) { start_join(""); }
        else { s_mode = KEYB; }
        kk_tone(660, 15);
    }
}

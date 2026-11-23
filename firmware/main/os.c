/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file os.c
 * @brief Kode-watch OS: kodeOS's shell on the wrist.
 *
 * Built after kodeOS on the Kode Dot (the same 410x502 glass): its palette,
 * its type (Outfit, DM Sans), its glyphs, the face with the time top left and
 * the battery's pile top right, the launcher's rail (112 px rows, the focus
 * line at 251, the ring that closes green round a launching app), the control
 * centre (six circles and the levels), and the back bar of a page.
 *
 *   Face:     swipe up or TOP = apps; swipe down = control centre.
 *   Apps:     drag the rail, tap a row to launch; swipe right or BOT = face.
 *   Control:  tap a circle, drag a level; swipe up, BOT or TOP = face.
 *   App:      the back bar, BOT or a swipe right = apps; TOP = face.
 *   Rest:     after a while the glass goes dark: the time, dim, once a minute,
 *             nudged against burn-in. A touch, a button or a notice wakes it.
 *
 * The Kode Link is the kodewatch app's (see the docs page):
 *   in:  notify <text> | find | go <0..3> | next | prev | timer add <s> |
 *        timer toggle | timer reset | home <0..3> | sys dock <0|1> | hub peer <0|1> | clear
 *   out: st <hh:mm> <bat> <dock> <screen> <steps> <notifs> <timer-s> <timer-run> <home-bits>
 *        home <name> <0|1>
 */
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "board.h"
#include "kfont.h"
#include "kk.h"
#include "kody.h"
#include "storage.h"
#include "net.h"
#include "wifi_ui.h"
#include "ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "apps.h"
#include "craft.h"
#include "sound.h"
#include "imu.h"
#include "ble.h"
#include "rlog.h"
#include "crash.h"
#include "fs.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "sdkconfig.h"

#define KW_VERSION "0.30.3"

#define CX (KK_W / 2)
#define NMAX 8
#define NLEN 256
#define PI2 6.2831853f

/* kodeOS's palette (kui_palette.h) */
#define C_SOFT  KK_RGB(0x19, 0x19, 0x19)
#define C_LIGHT KK_RGB(0xDE, 0xD7, 0xCD)
#define C_MED   KK_RGB(0xC2, 0xBA, 0xB3)
#define C_YEL   KK_RGB(0xFF, 0xAC, 0x00)
#define C_BLUE  KK_RGB(0x02, 0x8A, 0xC2)
#define C_ROW   KK_RGB(0x21, 0x21, 0x21)
#define C_OFF   KK_RGB(0x2A, 0x2A, 0x2A)
#define C_REST  KK_RGB(0x5C, 0x5A, 0x56)
#define C_OK    KK_RGB(0x31, 0xC4, 0x5A)
#define C_CHG   KK_RGB(0x67, 0xEA, 0x94)
#define C_RED   KK_RGB(0xC9, 0x30, 0x3D)

static uint16_t *g_fb;
static void wake(void);
static void sl_tick(uint32_t now);
static void bt_tick(uint32_t now);
static void alarm_tick(uint32_t now);
static bool draw_alarm_ring(uint32_t now);
static bool tap_alarm_ring(int x, int y, uint32_t now);
static void alarm_stop(bool snooze);
static void draw_face_kody(uint32_t now);
static void fl_open(const char *dir);
static int fo_off;
static void os_open_named(const char *nm);
static bool ble_asking(void);

/* ---- apps ---- */

enum { A_NOTIF, A_TIMER, A_HOME,
#if CONFIG_BT_ENABLED
       A_PHONE,
#endif
       A_RC, A_CRAFT, A_STORE, A_FILES, A_FOLDER, A_SET, A_STEPS, A_ALARM, A_CAL, A_MUSIC, A_FINDPH, A_SLEEP, A_BATT, A_WIFI, A_INFO, A_N };
typedef struct {
    const char *name;
    uint16_t disc;
    uint16_t ink;
    const kf_icon_t *glyph;
} app_t;
static const app_t APPS[A_N] = {
    { "Notifications", C_BLUE, C_SOFT, &KI_LIST_40 },
    { "Timer", C_YEL, C_SOFT, &KI_CLOCK_40 },
    { "Home", C_OK, C_SOFT, &KI_HOME_40 },
#if CONFIG_BT_ENABLED
    { "iPhone", C_BLUE, C_SOFT, &KI_BT_46 },
#endif
    { "RC car", C_RED, C_LIGHT, &KI_GAUGE_40 },
    { "Craft", C_OK, C_SOFT, &KI_PIN_40 },
    { "Store", C_BLUE, C_LIGHT, &KI_SHARE_46 },
    { "Files", C_YEL, C_SOFT, &KI_LIST_40 },
    { "Apps", C_RED, C_LIGHT, &KI_MORE_46 },
    { "Settings", C_MED, C_SOFT, &KI_SUN3_52 },
    { "Steps", C_OK, C_SOFT, &KI_HEART_40 },
    { "Alarm", C_YEL, C_SOFT, &KI_CLOCK_40 },
    { "Agenda", C_BLUE, C_SOFT, &KI_NOTE_40 },
    { "Music", C_RED, C_LIGHT, &KI_NOTE_40 },
    { "Find iPhone", C_BLUE, C_LIGHT, &KI_PIN_40 },
    { "Sleep", C_MED, C_SOFT, &KI_SUN1_52 },
    { "Battery", C_OK, C_SOFT, &KI_POWER_46 },
    { "Wi-Fi", C_MED, C_SOFT, &KI_WIFI_46 },
    { "Info", C_OFF, C_LIGHT, &KI_MORE_46 },
};

enum { V_FACE, V_APPS, V_CC, V_APP };
static int view = V_FACE;
static int app;

/* Room names: the hub sends its own ("hn <i> <name>"), from its Home Assistant map. */
static char ROOM[4][20] = { "Living room", "Kitchen", "Bedroom", "All off" };
static const char *const DAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- the model ---- */

static bool docked, linked;
static int battery = -1;
static bool charging;
EXT_RAM_BSS_ATTR static char notes[NMAX][NLEN];
static int note_open = -1;     /* the notification being read, -1 = the list */
static uint32_t note_at[NMAX];
static uint32_t note_uid[NMAX];        /* the iPhone's, 0 for the hub's */
static char note_app[NMAX][16];
static uint32_t call_uid;
static char call_name[48];
static uint32_t call_ring;
static int nnotes;
static uint32_t banner_until, find_until;
static int timer_set = 300, timer_left = 300;
static bool timer_run;
static uint32_t timer_tick, alarm_until;
static bool home_on[3];
static uint32_t last_status, last_sim;
static bool last_power;

/* settings, kept in NVS */
static const int REST_S[5] = { 5, 10, 30, 60, 0 };   /* 0 = never */
static int set_bright = 100;                         /* 10..100 */
static int set_rest = 1;
static bool set_raise = true, set_tapwake = true, set_aod = true;
static esp_pm_lock_handle_t s_fast;   /* the CPU at full speed while the screen is in use */
static bool rest_dark;
static int set_goal = 8000;
static int set_face;   /* the watch face (see draw_face) */
static int al_h = 7, al_m = 30;
static bool al_on, al_wd;            /* on; weekdays only */
static uint32_t al_ring_until;       /* ringing until (ms), 0 = quiet */
static long al_done_day = -1;        /* the day key it last rang on */
static int al_snooze_h = -1, al_snooze_m;
#define CAL_MAX 14
typedef struct { long ymd; int s, e; int c; char t[40]; } cal_ev_t;   /* s/e minutes of the day, -1 = all day */
static cal_ev_t cal_ev[CAL_MAX], cal_in[CAL_MAX];
static volatile int cal_n;
static int cal_nin;
static bool cal_ok;
static int cal_off;
static const uint16_t CAL_COL[4] = { C_BLUE, C_OK, C_YEL, C_RED };
static cal_ev_t const *cal_next(void);
static long cal_today(kk_clock_t const *c);
static long cal_tomorrow(long ymd);
static int cal_wday(int y, int m, int d);
static void cal_when(cal_ev_t const *e, char *o, size_t cap);
static void fit_text(const kf_font_t *f, char *s, int w);
/* the music player the hub follows: "mu <state> <vol> <i> <n> <player>|<title>|<artist>" */
static char mu_state[12] = "", mu_player[32] = "", mu_title[64] = "", mu_artist[48] = "";
static int mu_vol = -1, mu_i, mu_n;
static bool mu_ok;
/* the iPhone, from Home Assistant's companion app: "ph <battery|-1> <where>" and "ph rang" */
static int ph_bat = -1;
static char ph_where[24] = "";
static bool ph_ok;
static uint32_t ph_rang_at;
/* sleep: the night from 20:00 to 12:00 in 5-minute buckets of movement (milli-g, 255 = charging, 0 = no data) */
#define SL_N 192
static uint8_t sl_b[SL_N];
static long sl_night = -1;        /* yyyymmdd of the evening the night started */
static int sl_last = -1;          /* the bucket that was filled last */
static uint32_t sl_dirty_at;
/* battery: one sample per 10 minutes for 24 h (bit 7 = charging, 0 = no data) */
#define BT_N 144
static uint8_t bt_h[BT_N];
static long bt_slot = -1;         /* the 10-minute slot (a local-time count) of the newest sample */
static bool wx_ok;
static int wx_t;          /* degrees, rounded */
static char wx_c[20];     /* HA condition: sunny, cloudy, rainy, ... */

/* rest */
static bool resting;
static char app_get_id[24];
static uint32_t shot_until, shot_last;
static int rem_x0, rem_y0, rem_x1, rem_y1;
static uint32_t rem_t0, rem_ms;
static bool rem_top, rem_bot;
#define STORE_BASE "https://hub.example.com/static/watchapps/"
static int st_rm = -1;
static int st_off;      /* the first row shown */
static int rc_thr, rc_str;
static bool rc_stick, rc_fast, rc_light;
static uint32_t rc_sent, rc_car_at;
static int rc_kmh10 = -1, rc_bat = -1;
static uint32_t last_input;
static int rest_drawn_min = -1, rest_drawn_notes = -1, rest_drawn_bat = -2;

/* the rail */
#define ROW_H 112
#define FOCUS_Y 251
static float rail_u, rail_target;      /* 0 = the first row on the focus line */

/* launching */
static bool launching;
static uint32_t launch_t0;
static int launch_app;
static int launch_from_y;
#define L_FLIGHT 180U
#define L_SPIN   300U
#define L_HOLD   160U
#define L_FADE   120U

/* the control centre */
static uint32_t off_armed_until;
static int drag_level;                  /* 0 none, 1 brightness, 2 rest */

static void wake(void);
static void save_settings(void);

/* ---- small drawing helpers ---- */

static void text(const kf_font_t *f, int x, int top, const char *s, uint16_t c) { (void)kf_draw_top(g_fb, f, x, top, s, c); }
static void text_c(const kf_font_t *f, int cx, int top, const char *s, uint16_t c) { kf_draw_c(g_fb, f, cx, top, s, c); }

/* @p src cut with "..." to fit @p maxw. */
static void fit(const kf_font_t *f, const char *src, int maxw, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%s", src);
    if ( kf_width(f, out) <= maxw ) { return; }
    size_t n = strlen(out);
    while ( (n > 0) && (kf_width(f, out) + kf_width(f, "...") > maxw) ) { out[--n] = '\0'; }
    (void)snprintf(out + n, cap - n, "...");
}

static float wrap(float a)
{
    while ( a < 0.0f ) { a += PI2; }
    while ( a >= PI2 ) { a -= PI2; }
    return a;
}

/* A ring from @p a0 (radians, from 12 o'clock, clockwise) over @p sweep. */
/* Is the ray at angle @p th (0 up, clockwise) crossing row dy between dx0 and dx1? */
static bool ray_in_span(float th, int dy, int dx0, int dx1)
{
    float const sn = sinf(th), cs = cosf(th);
    if ( fabsf(cs) < 1e-4f ) { return (0 == dy) && (((sn > 0.0f) && (dx1 >= 0)) || ((sn < 0.0f) && (dx0 <= 0))); }
    float const t = -(float)dy / cs;
    if ( t < 0.0f ) { return false; }
    float const x = t * sn;
    return (x >= (float)dx0 - 1.0f) && (x <= (float)dx1 + 1.0f);
}

static void arc_span(int cx, int y, int dy, int dx0, int dx1, float a0, float sweep, bool full, uint16_t c)
{
    if ( dx0 < -cx ) { dx0 = -cx; }
    if ( dx1 > KK_W - 1 - cx ) { dx1 = KK_W - 1 - cx; }
    if ( dx0 > dx1 ) { return; }
    uint16_t *const row = &g_fb[y * KK_W + cx];
    if ( full || (!ray_in_span(a0, dy, dx0, dx1) && !ray_in_span(a0 + sweep, dy, dx0, dx1)) )
    {
        /* no edge of the arc in this run: all in or all out, as its middle is */
        int const m = (dx0 + dx1) / 2;
        if ( !full && (wrap(atan2f((float)m, (float)-dy) - a0) > sweep) ) { return; }
        for ( int dx = dx0; dx <= dx1; ++dx ) { row[dx] = c; }
        return;
    }
    for ( int dx = dx0; dx <= dx1; ++dx )
    {
        if ( wrap(atan2f((float)dx, (float)-dy) - a0) <= sweep ) { row[dx] = c; }
    }
}

/* A ring from rin to rout, from angle a0 (0 = up, clockwise) over sweep; run by run. */
static void arc(int cx, int cy, int rin, int rout, float a0, float sweep, uint16_t c)
{
    bool const full = sweep >= PI2 - 0.001f;
    if ( sweep <= 0.0f ) { return; }
    int const ri2 = rin * rin, ro2 = rout * rout;
    for ( int dy = -rout; dy <= rout; ++dy )
    {
        int const y = cy + dy;
        if ( (y < 0) || (y >= KK_H) ) { continue; }
        int const xo = (int)sqrtf((float)(ro2 - dy * dy));
        if ( dy * dy >= ri2 )
        {
            arc_span(cx, y, dy, -xo, xo, a0, sweep, full, c);
        }
        else
        {
            int const xi = (int)ceilf(sqrtf((float)(ri2 - dy * dy)));
            arc_span(cx, y, dy, -xo, -xi, a0, sweep, full, c);
            arc_span(cx, y, dy, xi, xo, a0, sweep, full, c);
        }
    }
}

/* A disc filled from below to @p pct, like the battery's liquid. */
static void liquid(int cx, int cy, int r, int pct, uint16_t c)
{
    int const top = cy + r - (2 * r * pct) / 100;
    for ( int dy = -r; dy <= r; ++dy )
    {
        int const y = cy + dy;
        if ( y < top ) { continue; }
        int const hw = (int)sqrtf((float)(r * r - dy * dy));
        kk_rect(cx - hw, y, 2 * hw + 1, 1, c);
    }
}

/* Darken a band of rows towards black: the fade under a bar. */
static void fade_rows(int y0, int y1, bool down)
{
    for ( int y = y0; y < y1; ++y )
    {
        if ( (y < 0) || (y >= KK_H) ) { continue; }
        int const t = ((down ? (y1 - y) : (y - y0)) * 256) / (y1 - y0);
        uint16_t *const row = &g_fb[y * KK_W];
        for ( int x = 0; x < KK_W; ++x ) { row[x] = kk_mix(row[x], KK_BLACK, t); }
    }
}

static void pill(int x, int y, int w, int h, const char *lbl, uint16_t bg, uint16_t fg)
{
    kk_round(x, y, w, h, h / 2, bg);
    text_c(&KF_NAME, x + w / 2, y + (h - KF_NAME.cap) / 2, lbl, fg);
}

static bool hit(int tx, int ty, int x, int y, int w, int h) { return (tx >= x) && (tx < x + w) && (ty >= y) && (ty < y + h); }

/* kodeOS's back bar: a chevron and the page's name, black above a short fade. */
#define BAR_FOOT 76
static void back_bar(const char *title)
{
    kk_rect(0, 0, KK_W, 56, KK_BLACK);
    fade_rows(56, BAR_FOOT, true);
    kk_line(48, 34, 38, 24, 4, C_LIGHT);
    kk_line(38, 24, 48, 14, 4, C_LIGHT);
    text(&KF_NAME, 62, 34 - KF_NAME.cap / 2, title, C_LIGHT);
}

/* ---- the model ---- */

static void add_note_from(const char *from, uint32_t uid, const char *t)
{
    if ( nnotes == NMAX )
    {
        memmove(notes[0], notes[1], sizeof(notes[0]) * (NMAX - 1));
        memmove(&note_at[0], &note_at[1], sizeof(note_at[0]) * (NMAX - 1));
        memmove(&note_uid[0], &note_uid[1], sizeof(note_uid[0]) * (NMAX - 1));
        memmove(note_app[0], note_app[1], sizeof(note_app[0]) * (NMAX - 1));
        --nnotes;
    }
    (void)snprintf(notes[nnotes], NLEN, "%s", t);
    (void)snprintf(note_app[nnotes], sizeof(note_app[0]), "%s", from);
    note_uid[nnotes] = uid;
    note_at[nnotes] = kk_ms();
    ++nnotes;
    banner_until = kk_ms() + 3500;
    if ( 0 != strcmp(t, "Timer done") ) { sound_play(SND_NOTE); }
    kk_buzz(250);
    kody_play(KODY_NOTICE);
    wake();
}

static void add_note(const char *t) { add_note_from("", 0, t); }

static void del_note(int i)
{
    if ( (i < 0) || (i >= nnotes) ) { return; }
    memmove(notes[i], notes[i + 1], sizeof(notes[0]) * (size_t)(nnotes - i - 1));
    memmove(&note_at[i], &note_at[i + 1], sizeof(note_at[0]) * (size_t)(nnotes - i - 1));
    memmove(&note_uid[i], &note_uid[i + 1], sizeof(note_uid[0]) * (size_t)(nnotes - i - 1));
    memmove(note_app[i], note_app[i + 1], sizeof(note_app[0]) * (size_t)(nnotes - i - 1));
    --nnotes;
}

/* the iPhone's lines (ble.h) */
static void on_ble(char *m)
{
    unsigned long uid = 0;
    int n = 0;
    if ( (1 == sscanf(m, "bnote %lu %n", &uid, &n)) && n )
    {
        char *app = m + n, *title = strchr(app, '|'), *msg = NULL;
        if ( title ) { *title++ = '\0'; msg = strchr(title, '|'); }
        if ( msg ) { *msg++ = '\0'; }
        char t[NLEN];
        if ( title && title[0] && msg && msg[0] ) { (void)snprintf(t, sizeof(t), "%s: %s", title, msg); }
        else { (void)snprintf(t, sizeof(t), "%s", (msg && msg[0]) ? msg : (title && title[0]) ? title : app); }
        add_note_from(app, (uint32_t)uid, t);
    }
    else if ( (1 == sscanf(m, "bcall %lu %n", &uid, &n)) && n )
    {
        call_uid = (uint32_t)uid;
        (void)snprintf(call_name, sizeof(call_name), "%s", m + n);
        call_ring = 0;
        wake();
    }
    else if ( 1 == sscanf(m, "bnrm %lu", &uid) )
    {
        if ( call_uid == (uint32_t)uid ) { call_uid = 0; }
        for ( int i = nnotes - 1; i >= 0; --i ) { if ( note_uid[i] == (uint32_t)uid ) { del_note(i); } }
    }
    else if ( 0 == strncmp(m, "btime ", 6) ) { kk_set_local_time((int64_t)strtoll(m + 6, NULL, 10)); }
    else if ( 0 == strcmp(m, "bpair") ) { wake(); }
}

static void timer_toggle(void)
{
    if ( timer_left <= 0 ) { timer_left = timer_set; }
    timer_run = !timer_run;
    timer_tick = kk_ms();
}

static void timer_add(int s)
{
    timer_left += s;
    if ( timer_left > 99 * 60 ) { timer_left = 99 * 60; }
    if ( !timer_run ) { timer_set = timer_left; }
}

static void home_toggle(int i)
{
    if ( 3 == i )
    {
        for ( int k = 0; k < 3; ++k ) { home_on[k] = false; }
        kk_link_send("home 3 0");
    }
    else if ( (i >= 0) && (i < 3) )
    {
        /* Shown at once; the hub answers with Home Assistant's own state ("hs"). */
        home_on[i] = !home_on[i];
        kk_link_send("home %d %d", i, home_on[i] ? 1 : 0);
    }
}

static void open_app(int a)
{
    if ( (a < 0) || (a >= A_N) ) { return; }
    app = a;
    view = V_APP;
    if ( A_WIFI == a ) { wifi_ui_back(); }
    if ( A_FILES == a ) { fl_open(""); }
    if ( A_CAL == a ) { cal_off = 0; }
    if ( A_NOTIF == a ) { note_open = -1; }
    if ( A_MUSIC == a ) { kk_link_send("mu ?"); }
    if ( A_FINDPH == a ) { kk_link_send("ph ?"); }
    if ( A_FOLDER == a ) { fo_off = 0; apps_refresh(STORE_BASE "index.txt"); }
    if ( A_STORE == a ) { st_rm = -1; st_off = 0; apps_refresh(STORE_BASE "index.txt"); }
}

static void go_screen(int s)
{
    s = ((s % 4) + 4) % 4;
    if ( 0 == s ) { view = V_FACE; }
    else { open_app((1 == s) ? A_NOTIF : (2 == s) ? A_TIMER : A_HOME); }
    wake();
}

static int cur_screen(void)
{
    if ( V_APP != view ) { return 0; }
    return (A_NOTIF == app) ? 1 : (A_TIMER == app) ? 2 : (A_HOME == app) ? 3 : 0;
}

static void on_link(const char *m)
{
    int a = 0;
    if ( fs_on_line(m) ) { return; }
    if ( 0 == strncmp(m, "os open ", 8) ) { os_open_named(m + 8); return; }
    if ( 0 == strcmp(m, "alarm ring") ) { al_ring_until = kk_ms() + 120000U; view = V_FACE; wake(); return; }
    {
        float t = 0.0f;
        char c[20];
        if ( 2 == sscanf(m, "wx %f %19s", &t, c) )
        {
            wx_t = (int)lroundf(t);
            (void)snprintf(wx_c, sizeof(wx_c), "%s", c);
            wx_ok = true;
            return;
        }
    }
    if ( 0 == strncmp(m, "ph ", 3) )
    {
        int b = -1, k = 0;
        if ( 0 == strncmp(m + 3, "rang", 4) ) { ph_rang_at = kk_ms(); }
        else if ( (1 == sscanf(m + 3, "%d %n", &b, &k)) && (k > 0) )
        {
            ph_bat = b;
            (void)snprintf(ph_where, sizeof(ph_where), "%s", m + 3 + k);
            ph_ok = true;
        }
        return;
    }
    if ( 0 == strncmp(m, "mu ", 3) )
    {
        char st[12];
        int v = -1, i = 0, n = 0, k = 0;
        if ( (4 == sscanf(m + 3, "%11s %d %d %d %n", st, &v, &i, &n, &k)) && (k > 0) )
        {
            const char *p = m + 3 + k;
            const char *const a = strchr(p, '|');
            const char *const b = a ? strchr(a + 1, '|') : NULL;
            (void)snprintf(mu_state, sizeof(mu_state), "%s", st);
            mu_vol = v;
            mu_i = i;
            mu_n = n;
            (void)snprintf(mu_player, sizeof(mu_player), "%.*s", a ? (int)(a - p) : (int)strlen(p), p);
            (void)snprintf(mu_title, sizeof(mu_title), "%.*s", (a && b) ? (int)(b - a - 1) : 0, a ? a + 1 : "");
            (void)snprintf(mu_artist, sizeof(mu_artist), "%s", b ? b + 1 : "");
            mu_ok = true;
        }
        return;
    }
    if ( 0 == strncmp(m, "cal ", 4) )
    {
        cal_ev_t e = { 0 };
        int n = 0;
        if ( 0 == strncmp(m + 4, "clr", 3) ) { cal_nin = 0; }
        else if ( 0 == strncmp(m + 4, "end", 3) )
        {
            ESP_LOGI("os", "agenda: %d events", cal_nin);
            cal_n = 0;
            memcpy(cal_ev, cal_in, sizeof(cal_in[0]) * (size_t)cal_nin);
            cal_n = cal_nin;
            cal_ok = true;
        }
        else if ( (4 == sscanf(m + 4, "%ld %d %d %d %n", &e.ymd, &e.s, &e.e, &e.c, &n)) && (n > 0) && (cal_nin < CAL_MAX) )
        {
            (void)snprintf(e.t, sizeof(e.t), "%s", m + 4 + n);
            cal_in[cal_nin++] = e;
        }
        return;
    }
    if ( 0 == strncmp(m, "notify ", 7) ) { add_note(m + 7); }
    else if ( 0 == strcmp(m, "find") ) { find_until = kk_ms() + 4000; wake(); }
    else if ( 1 == sscanf(m, "go %d", &a) ) { go_screen(a); }
    else if ( 0 == strcmp(m, "next") ) { go_screen(cur_screen() + 1); }
    else if ( 0 == strcmp(m, "prev") ) { go_screen(cur_screen() - 1); }
    else if ( 1 == sscanf(m, "timer add %d", &a) ) { timer_add(a); }
    else if ( 0 == strcmp(m, "timer toggle") ) { timer_toggle(); }
    else if ( 0 == strcmp(m, "timer reset") ) { timer_run = false; timer_left = timer_set = 300; }
    else if ( 1 == sscanf(m, "home %d", &a) ) { home_toggle(a); }
    else if ( 0 == strncmp(m, "ota ", 4) ) { (void)ota_start(m + 4); wake(); }
    else if ( 0 == strcmp(m, "shot") ) { shot_until = kk_ms() + 6000U; }
    else if ( 2 == sscanf(m, "rtap %d %d", &rem_x0, &rem_y0) ) { rem_x1 = rem_x0; rem_y1 = rem_y0; rem_t0 = kk_ms(); rem_ms = 120; wake(); }
    else if ( 4 == sscanf(m, "rswipe %d %d %d %d", &rem_x0, &rem_y0, &rem_x1, &rem_y1) ) { rem_t0 = kk_ms(); rem_ms = 260; wake(); }
    else if ( 0 == strcmp(m, "rbtn top") ) { rem_top = true; }
    else if ( 0 == strcmp(m, "rbtn bot") ) { rem_bot = true; }
    else if ( 0 == strncmp(m, "app open ", 9) ) { (void)apps_open(m + 9); }
    else if ( 0 == strncmp(m, "app remove ", 11) ) { kk_link_send("app removed %s %d", m + 11, apps_remove(m + 11) ? 1 : 0); }
    else if ( 0 == strncmp(m, "app get ", 8) )
    {
        /* after the catalog is loaded: rc_tick-style, in the main loop */
        (void)snprintf(app_get_id, sizeof(app_get_id), "%s", m + 8);
        apps_refresh(STORE_BASE "index.txt");
    }
    else if ( 0 == strncmp(m, "kody ", 5) )
    {
        /* "kody <url> <sha256>": Kody's frames over Wi-Fi */
        char url[200], sha[65];
        if ( 2 == sscanf(m + 5, "%199s %64s", url, sha) ) { (void)ota_kody(url, sha); wake(); }
    }
    else if ( 0 == strncmp(m, "car ", 4) )
    {
        int k = 0, b = 0, f = 0;
        if ( 3 == sscanf(m + 4, "%d %d %d", &k, &b, &f) ) { rc_kmh10 = k; rc_bat = b; rc_fast = (0 != f); rc_car_at = kk_ms(); }
    }
    else if ( 1 == sscanf(m, "hs %d", &a) ) { for ( int k = 0; k < 3; ++k ) { home_on[k] = ((a >> k) & 1) != 0; } }
    else if ( (0 == strncmp(m, "hn ", 3)) && (m[3] >= '0') && (m[3] <= '3') && (' ' == m[4]) )
    {
        (void)snprintf(ROOM[m[3] - '0'], sizeof(ROOM[0]), "%s", m + 5);
    }
    else if ( 1 == sscanf(m, "sys dock %d", &a) ) { docked = (a != 0); }
    else if ( 1 == sscanf(m, "hub peer %d", &a) ) { linked = (a != 0); }
    else if ( 0 == strcmp(m, "clear") ) { nnotes = 0; }
}

/* ---- RC car: a one-finger stick, sent to the car through the hub ----
 * Link out: "drive <throttle> <steer>" (-100..100) twenty times a second while the
 * app is open (the car stops after 300 ms of silence), "fast 0|1", "light 0|1".
 * Link in: "car <km/h x10> <battery %> <fast> <source>" from the car. */
#define RC_CY   262
#define RC_R    118
#define RC_BY   410

static int clamp100(int v) { return (v > 100) ? 100 : (v < -100) ? -100 : v; }

static void rc_stick_at(int x, int y)
{
    rc_str = clamp100(((x - CX) * 100) / RC_R);
    rc_thr = clamp100(((RC_CY - y) * 100) / RC_R);
}

static void rc_tick(uint32_t now)
{
    bool const open = (V_APP == view) && (A_RC == app) && !resting;
    static bool was_open;
    if ( !open )
    {
        if ( was_open ) { kk_link_send("drive 0 0"); net_low_latency(false); }
        was_open = false;
        rc_stick = false;
        rc_thr = rc_str = 0;
        return;
    }
    if ( !was_open ) { net_low_latency(true); }
    was_open = true;
    if ( (now - rc_sent) >= 50U )
    {
        rc_sent = now;
        kk_link_send("drive %d %d", rc_stick ? rc_thr : 0, rc_stick ? rc_str : 0);
    }
}

static void draw_rc(uint32_t now)
{
    bool const car = (0U != rc_car_at) && ((now - rc_car_at) < 1500U);
    char v[24];
    if ( car && (rc_kmh10 >= 0) )
    {
        (void)snprintf(v, sizeof(v), "%d.%d", rc_kmh10 / 10, rc_kmh10 % 10);
        text_c(&KF_FIGURE, CX, 96, v, C_LIGHT);
        (void)snprintf(v, sizeof(v), "km/h   battery %d%%", rc_bat);
        text_c(&KF_CAPTION, CX, 128, v, C_MED);
    }
    else
    {
        text_c(&KF_NAME, CX, 100, net_hub_up() ? "no car" : "hub not connected", C_MED);
        text_c(&KF_CAPTION, CX, 128, "open hub.example.com/rc", C_REST);
    }
    kk_disc(CX, RC_CY, RC_R + 6, C_ROW);
    kk_disc(CX, RC_CY, RC_R - 50, C_SOFT);
    int const kx = CX + (rc_stick ? (rc_str * RC_R) / 100 : 0);
    int const ky = RC_CY - (rc_stick ? (rc_thr * RC_R) / 100 : 0);
    kk_disc(kx, ky, 34, rc_stick ? C_RED : C_OFF);
    kk_round(24, RC_BY, 172, 64, 32, rc_fast ? C_YEL : C_ROW);
    text_c(&KF_NAME, 24 + 86, RC_BY + 32 - KF_NAME.cap / 2, rc_fast ? "Fast" : "Safe", rc_fast ? C_SOFT : C_LIGHT);
    kk_round(KK_W - 24 - 172, RC_BY, 172, 64, 32, rc_light ? C_BLUE : C_ROW);
    text_c(&KF_NAME, KK_W - 24 - 86, RC_BY + 32 - KF_NAME.cap / 2, "Lights", C_LIGHT);
    back_bar("RC car");
}

static void tap_rc(int x, int y)
{
    if ( hit(x, y, 24, RC_BY, 172, 64) )
    {
        rc_fast = !rc_fast;
        kk_link_send("fast %d", rc_fast ? 1 : 0);
    }
    else if ( hit(x, y, KK_W - 24 - 172, RC_BY, 172, 64) )
    {
        rc_light = !rc_light;
        kk_link_send("light %d", rc_light ? 1 : 0);
    }
}

/* ---- Store: apps from the hub's catalog, into the apps region ---- */
#define ST_Y0 120
#define ST_ROW 90
#define ST_ROWS 4
EXT_RAM_BSS_ATTR static apps_item_t st_items[APPS_MAX];
static int st_n;
static uint32_t st_rm_until;

static void draw_store(uint32_t now)
{
    st_n = apps_list(st_items, APPS_MAX);
    char v[48];
    if ( apps_progress() >= 0 ) { (void)snprintf(v, sizeof(v), "Working %d%%", apps_progress()); }
    else if ( apps_loading() ) { (void)snprintf(v, sizeof(v), "Loading the catalog..."); }
    else if ( apps_error()[0] ) { (void)snprintf(v, sizeof(v), "%s", apps_error()); }
    else if ( 0 == st_n ) { (void)snprintf(v, sizeof(v), net_hub_up() ? "No apps yet" : "Needs Wi-Fi"); }
    else if ( st_n > ST_ROWS ) { (void)snprintf(v, sizeof(v), "%d-%d of %d apps - swipe for more", st_off + 1, (st_off + ST_ROWS < st_n) ? st_off + ST_ROWS : st_n, st_n); }
    else { (void)snprintf(v, sizeof(v), "%d apps", st_n); }
    text_c(&KF_CAPTION, CX, 88, v, apps_error()[0] ? C_RED : C_MED);
    if ( (st_rm >= 0) && (now > st_rm_until) ) { st_rm = -1; }
    if ( st_off > st_n - ST_ROWS ) { st_off = (st_n > ST_ROWS) ? st_n - ST_ROWS : 0; }
    for ( int r = 0; (r < ST_ROWS) && (st_off + r < st_n); ++r )
    {
        int const i = st_off + r;
        apps_item_t const *const it = &st_items[i];
        int const y = ST_Y0 + r * ST_ROW;
        kk_round(18, y, KK_W - 36, ST_ROW - 8, 30, C_ROW);
        kk_disc(18 + 42, y + 41, 28, it->color);
        char l[2] = { it->name[0], '\0' };
        text_c(&KF_NAME, 18 + 42, y + 41 - KF_NAME.cap / 2, l, C_SOFT);
        char nm[24];
        fit(&KF_NAME, it->name, 150, nm, sizeof(nm));
        text(&KF_NAME, 18 + 84, y + 22, nm, C_LIGHT);
        (void)snprintf(v, sizeof(v), "%.1f MB", (double)it->size / (1024.0 * 1024.0));
        text(&KF_CAPTION, 18 + 84, y + 54, v, C_MED);
        bool const rm = (st_rm == i);
        uint16_t const bc = rm ? C_RED : it->installed ? C_OK : C_BLUE;
        kk_round(KK_W - 18 - 16 - 104, y + 21, 104, 40, 20, bc);
        text_c(&KF_BODY, KK_W - 18 - 16 - 52, y + 41 - KF_BODY.cap / 2, rm ? "Remove" : it->installed ? "Open" : "Get", C_LIGHT);
    }
    back_bar("Store");
}

static void tap_store(int x, int y, uint32_t now)
{
    for ( int r = 0; (r < ST_ROWS) && (st_off + r < st_n); ++r )
    {
        int const i = st_off + r;
        int const ry = ST_Y0 + r * ST_ROW;
        if ( !hit(x, y, 18, ry, KK_W - 36, ST_ROW - 8) ) { continue; }
        apps_item_t const *const it = &st_items[i];
        bool const button = (x >= KK_W - 18 - 16 - 104 - 10);
        if ( button && (st_rm == i) ) { (void)apps_remove(it->id); st_rm = -1; }
        else if ( button && it->installed ) { (void)apps_open(it->id); }
        else if ( button ) { (void)apps_install(i, STORE_BASE); }
        else if ( it->installed ) { st_rm = i; st_rm_until = now + 3000U; }   /* tap the name: offer Remove */
        return;
    }
}

/* ---- Craft: a Minecraft Java server (bareiron) ---- */
static void draw_craft(void)
{
    net_status_t st;
    net_status(&st);
    craft_state_t const cs = craft_state();
    text_c(&KF_TITLE, CX, 96, "Minecraft server", C_LIGHT);
    text_c(&KF_CAPTION, CX, 136, "Java edition 1.21.8, same Wi-Fi", C_MED);
    char v[48];
    if ( CRAFT_RUNNING == cs )
    {
        (void)snprintf(v, sizeof(v), "%s", st.ip[0] ? st.ip : "-");
        text_c(&KF_CAPTION, CX, 190, "Multiplayer > Direct connection:", C_MED);
        text_c(&KF_NAME, CX, 222, v, C_OK);
        (void)snprintf(v, sizeof(v), "%d %s", craft_players(), (1 == craft_players()) ? "player" : "players");
        text_c(&KF_FIGURE, CX, 280, v, C_LIGHT);
        text_c(&KF_CAPTION, CX, 420, "runs until the watch restarts", C_REST);
    }
    else
    {
        if ( CRAFT_FAILED == cs ) { text_c(&KF_NAME, CX, 220, "the server stopped", C_RED); }
        bool const ok = st.wifi_up;
        kk_round(CX - 130, 320, 260, 70, 35, ok ? C_OK : C_OFF);
        text_c(&KF_NAME, CX, 355 - KF_NAME.cap / 2, ok ? "Start server" : "Needs Wi-Fi", ok ? C_SOFT : C_MED);
        text_c(&KF_CAPTION, CX, 420, "bareiron by p2r3 (GPL-3.0)", C_REST);
    }
    back_bar("Craft");
}

static void tap_craft(int x, int y)
{
    net_status_t st;
    net_status(&st);
    if ( (CRAFT_RUNNING != craft_state()) && st.wifi_up && hit(x, y, CX - 130, 320, 260, 70) ) { (void)craft_start(); }
}

/* ---- the day's steps ---- */
static long st_day = -1, st_base;   /* st_base + imu_steps() = today */
static uint32_t st_saved_at;

static long today_steps(void) { return st_base + (long)imu_steps(); }

#define ST_HIST 7
static long st_hist[ST_HIST];
static bool st_hist_loaded;

static void hist_load(void)
{
    if ( st_hist_loaded ) { return; }
    st_hist_loaded = true;
    for ( int i = 0; i < ST_HIST; ++i )
    {
        char k[8];
        (void)snprintf(k, sizeof(k), "st_h%d", i);
        long const v = kk_best_get(k);
        st_hist[i] = (v > 0) ? v : 0;
    }
}

/* a day ended with @p total steps: it becomes yesterday */
static void hist_push(long total)
{
    hist_load();
    for ( int i = ST_HIST - 1; i > 0; --i ) { st_hist[i] = st_hist[i - 1]; }
    st_hist[0] = (total > 0) ? total : 0;
    for ( int i = 0; i < ST_HIST; ++i )
    {
        char k[8];
        (void)snprintf(k, sizeof(k), "st_h%d", i);
        kk_best_put(k, st_hist[i]);
    }
}

static void steps_tick(uint32_t now)
{
    kk_clock_t c;
    if ( !kk_clock(&c) ) { return; }
    long const key = (long)c.year * 400L + (long)c.mon * 32L + (long)c.mday;
    if ( st_day < 0 )
    {
        /* first known day: take over what this day had before the restart */
        long const saved = kk_best_get("st_day");
        st_day = key;
        if ( saved == key ) { st_base = kk_best_get("st_n"); }
        else
        {
            if ( saved > 0 ) { hist_push(kk_best_get("st_n")); }   /* the day before the restart */
            st_base = 0;
            kk_best_put("st_day", st_day);
            kk_best_put("st_n", 0);
        }
    }
    else if ( key != st_day )
    {
        hist_push(today_steps());
        st_day = key;
        st_base = -(long)imu_steps();
        kk_best_put("st_day", st_day);
        kk_best_put("st_n", 0);
    }
    if ( (now - st_saved_at) > 300000U )
    {
        st_saved_at = now;
        kk_best_put("st_day", st_day);
        kk_best_put("st_n", today_steps());
    }
}

/* ---- remote: the hub's preview and control (/oud/update) ----
 * In: "shot" (stream the screen for 6 s more), "rtap x y", "rswipe x0 y0 x1 y1",
 * "rbtn top|bot", "app open <id>". Out: binary frames "KWS1" w h, then
 * (count u16, rgb565 u16) runs over the screen at half size. */
#define SHOT_W (KK_W / 2)
#define SHOT_H (KK_H / 2)
#define SHOT_CAP (8 + SHOT_W * SHOT_H * 4)
static uint8_t *shot_buf;
static volatile size_t shot_len;
static SemaphoreHandle_t shot_go;

static void shot_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        if ( pdTRUE != xSemaphoreTake(shot_go, portMAX_DELAY) ) { continue; }
        (void)net_send_bin(shot_buf, shot_len);
        shot_len = 0;
    }
}

void kk_frame_hook(const uint16_t *fb)
{
    uint32_t const now = kk_ms();
    if ( (shot_until < now) || (0 != shot_len) || ((now - shot_last) < 400U) || !net_hub_up() ) { return; }
    if ( NULL == shot_buf )
    {
        shot_buf = heap_caps_malloc(SHOT_CAP, MALLOC_CAP_SPIRAM);
        shot_go = xSemaphoreCreateBinary();
        if ( (NULL == shot_buf) || (NULL == shot_go) || (pdPASS != xTaskCreatePinnedToCore(shot_task, "shot", 4096, NULL, 2, NULL, 0)) ) { return; }
    }
    shot_last = now;
    uint8_t *o = shot_buf;
    memcpy(o, "KWS1", 4);
    o[4] = (uint8_t)SHOT_W; o[5] = (uint8_t)(SHOT_W >> 8); o[6] = (uint8_t)SHOT_H; o[7] = (uint8_t)(SHOT_H >> 8);
    o += 8;
    uint16_t run_c = 0, run_n = 0;
    for ( int y = 0; y < SHOT_H; ++y )
    {
        const uint16_t *r = &fb[(y * 2) * KK_W];
        for ( int x = 0; x < SHOT_W; ++x )
        {
            uint16_t const c = r[x * 2];
            if ( (run_n > 0) && ((c != run_c) || (0xFFFF == run_n)) )
            {
                o[0] = (uint8_t)run_n; o[1] = (uint8_t)(run_n >> 8); o[2] = (uint8_t)run_c; o[3] = (uint8_t)(run_c >> 8);
                o += 4;
                run_n = 0;
            }
            run_c = c;
            ++run_n;
        }
    }
    o[0] = (uint8_t)run_n; o[1] = (uint8_t)(run_n >> 8); o[2] = (uint8_t)run_c; o[3] = (uint8_t)(run_c >> 8);
    o += 4;
    shot_len = (size_t)(o - shot_buf);
    (void)xSemaphoreGive(shot_go);
}

/** @brief Where a finger from the hub is now, if one is down. */
static bool remote_touch(uint32_t now, int *x, int *y)
{
    if ( (0U == rem_ms) || (now - rem_t0) >= rem_ms ) { return false; }
    int const k = (int)(((now - rem_t0) * 1000U) / rem_ms);
    *x = rem_x0 + ((rem_x1 - rem_x0) * k) / 1000;
    *y = rem_y0 + ((rem_y1 - rem_y0) * k) / 1000;
    return true;
}

static void tick(uint32_t now)
{
    if ( (now - last_sim) < 1000U ) { return; }
    last_sim = now;
    battery = kk_battery();
    charging = kk_charging();
    steps_tick(now);
    {
        kk_clock_t hc;
        kody_set_hour(kk_clock(&hc) ? hc.hour : -1);
    }
    bool const p = kk_power_in();
    if ( p != last_power )
    {
        last_power = p;
        docked = p;
        if ( p ) { kody_play(KODY_PLUGGED); }
    }
    if ( timer_run )
    {
        while ( (now - timer_tick) >= 1000U )
        {
            timer_tick += 1000U;
            if ( --timer_left <= 0 )
            {
                timer_left = 0;
                timer_run = false;
                alarm_until = now + 30000;
                add_note("Timer done");
                break;
            }
        }
    }
}

static void send_status(uint32_t now)
{
    if ( (now - last_status) < 1000U ) { return; }
    last_status = now;
    kk_clock_t c;
    char hm[8] = "--:--";
    if ( kk_clock(&c) ) { (void)snprintf(hm, sizeof(hm), "%02d:%02d", c.hour, c.min); }
    kk_link_send("st %s %d %d %d %d %d %d %d %d", hm, (battery < 0) ? 0 : battery, docked ? 1 : 0, cur_screen(), (int)((st_day >= 0) ? today_steps() : 0),
                 nnotes, timer_left, timer_run ? 1 : 0,
                 (home_on[0] ? 1 : 0) | (home_on[1] ? 2 : 0) | (home_on[2] ? 4 : 0));
}

/* ---- settings and rest ---- */

static void apply_bright(void) { kk_bright(resting ? (docked ? 20 : 10) : set_bright); }

static void load_settings(void)
{
    long const b = kk_best_get("os_bright2"), r = kk_best_get("os_rest2");
    if ( (b >= 10) && (b <= 100) ) { set_bright = (int)b; }
    if ( r >= 1 ) { set_rest = (int)(r - 1) % 5; }
    sound_set_on(kk_best_get("os_sound") != 1);   /* 1 = off; unset or 2 = on */
    set_raise = (kk_best_get("os_raise") != 1);
    set_tapwake = (kk_best_get("os_tapw") != 1);
    set_aod = (kk_best_get("os_aod") != 1);
    long const g = kk_best_get("os_goal");
    if ( (g >= 1000) && (g <= 50000) ) { set_goal = (int)g; }
    long const ah = kk_best_get("os_al_h"), am = kk_best_get("os_al_m"), ao = kk_best_get("os_al_on");
    if ( (ah >= 1) && (ah <= 24) ) { al_h = (int)ah - 1; }
    if ( (am >= 1) && (am <= 60) ) { al_m = (int)am - 1; }
    al_on = (ao & 1) != 0;
    al_wd = (ao & 2) != 0;
    long const f = kk_best_get("os_face");
    if ( f >= 1 ) { set_face = (int)(f - 1); }
}

static void save_settings(void)
{
    kk_best_put("os_bright2", set_bright);
    kk_best_put("os_rest2", set_rest + 1);
    kk_best_put("os_sound", sound_on() ? 2 : 1);
    kk_best_put("os_raise", set_raise ? 2 : 1);
    kk_best_put("os_tapw", set_tapwake ? 2 : 1);
    kk_best_put("os_aod", set_aod ? 2 : 1);
    kk_best_put("os_goal", set_goal);
    kk_best_put("os_face", set_face + 1);
    kk_best_put("os_al_h", al_h + 1);
    kk_best_put("os_al_m", al_m + 1);
    kk_best_put("os_al_on", (al_on ? 1 : 0) | (al_wd ? 2 : 0) | 4);
}

static void wake(void)
{
    last_input = kk_ms();
    if ( resting )
    {
        resting = false;
        if ( s_fast ) { (void)esp_pm_lock_acquire(s_fast); }
        board_panel(true);
        rest_dark = false;
        net_rest(false);
        apply_bright();
        kody_play(KODY_WAKE);
    }
}

static void maybe_rest(uint32_t now)
{
    if ( resting || launching || drag_level ) { return; }
    int const secs = REST_S[set_rest];   /* the setting, on the wrist and in the dock alike */
    if ( 0 == secs ) { return; }
    if ( (V_APP == view) && (A_WIFI == app) && wifi_ui_modal() ) { return; }
    if ( (V_APP == view) && (A_RC == app) ) { return; }   /* driving: never rest under the finger */
    if ( (find_until > now) || (banner_until > now) || (alarm_until > now) ) { return; }
    if ( call_uid || ble_asking() ) { return; }
    if ( al_ring_until > now ) { return; }
    if ( ota_progress() >= 0 ) { return; }   /* updating: awake, and Wi-Fi at full speed */
    if ( (int32_t)(now - last_input) < secs * 1000 ) { return; }   /* signed: a wake after 'now' was taken is not 'long ago' */
    resting = true;
    view = V_FACE;
    rest_drawn_min = -1;
    rest_dark = false;
    if ( s_fast ) { (void)esp_pm_lock_release(s_fast); }
    net_rest(true);
    apply_bright();
}

/* ---- the face ---- */

static void pile(int x, int y, bool dim)
{
    /* kodeOS's battery: a 31x20 pile, a 2 px border, a 23x12 fill and its tip. */
    uint16_t const frame = dim ? C_REST : C_LIGHT;
    uint16_t fill = frame;
    if ( charging ) { fill = C_CHG; }
    else if ( (battery >= 0) && (battery < 20) ) { fill = C_RED; }
    kk_round(x, y, 31, 20, 5, frame);
    kk_round(x + 2, y + 2, 27, 16, 3, KK_BLACK);
    kk_round(x + 33, y + 7, 3, 7, 1, frame);
    int const w = (battery < 0) ? 0 : ((23 * battery) / 100 < 3 ? 3 : (23 * battery) / 100);
    if ( w ) { kk_round(x + 4, y + 4, w, 12, 2, fill); }
}

static void face_time(int x, int top, uint16_t col)
{
    kk_clock_t c;
    char t[8] = "--:--";
    if ( kk_clock(&c) ) { (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min); }
    text(&KF_CLOCK, x, top, t, col);
}

static int face_date(int x, int top, uint16_t col)
{
    kk_clock_t c;
    char t[24] = "";
    if ( kk_clock(&c) ) { (void)snprintf(t, sizeof(t), "%s %d %s", DAY[c.wday], c.mday, MON[c.mon - 1]); }
    else { (void)snprintf(t, sizeof(t), "set the time"); }
    text(&KF_BODY, x, top, t, col);
    return kf_width(&KF_BODY, t);
}

#define KODY_Y 168
static uint32_t kody_next_react;

/* ---- the weather's glyph and the temperature ---- */

static void wx_cloud(int cx, int cy, uint16_t c)
{
    kk_disc(cx - 8, cy + 2, 8, c);
    kk_disc(cx + 1, cy - 4, 10, c);
    kk_disc(cx + 10, cy + 3, 7, c);
    kk_round(cx - 16, cy + 2, 33, 9, 4, c);
}

static void wx_glyph(int cx, int cy)
{
    const char *const w = wx_c;
    if ( 0 == strcmp(w, "sunny") ) { kk_disc(cx, cy, 11, C_YEL); return; }
    if ( 0 == strcmp(w, "clear-night") ) { kk_disc(cx, cy, 10, C_LIGHT); kk_disc(cx + 5, cy - 4, 9, KK_BLACK); return; }
    if ( 0 == strcmp(w, "partlycloudy") ) { kk_disc(cx + 6, cy - 6, 8, C_YEL); wx_cloud(cx - 2, cy + 2, C_MED); return; }
    if ( (0 == strcmp(w, "rainy")) || (0 == strcmp(w, "pouring")) || (0 == strncmp(w, "lightning", 9)) )
    {
        wx_cloud(cx, cy - 4, C_MED);
        for ( int i = -1; i <= 1; ++i ) { kk_line(cx + i * 8, cy + 9, cx + i * 8 - 3, cy + 16, 2, C_BLUE); }
        return;
    }
    if ( (0 == strcmp(w, "snowy")) || (0 == strcmp(w, "snowy-rainy")) || (0 == strcmp(w, "hail")) )
    {
        wx_cloud(cx, cy - 4, C_MED);
        for ( int i = -1; i <= 1; ++i ) { kk_disc(cx + i * 8, cy + 13, 2, C_LIGHT); }
        return;
    }
    wx_cloud(cx, cy, (0 == strcmp(w, "fog")) ? C_REST : C_MED);   /* cloudy, fog, windy, exceptional */
}

/* glyph and "N°" from x (left) on the line whose middle is cy; returns the width */
static int wx_draw(int x, int cy, const kf_font_t *f, uint16_t col)
{
    if ( !wx_ok ) { return 0; }
    wx_glyph(x + 16, cy);
    char t[8];
    (void)snprintf(t, sizeof(t), "%d", wx_t);
    int const tx = x + 38;
    text(f, tx, cy - f->cap / 2, t, col);
    int const w = kf_width(f, t);
    kk_ring(tx + w + 6, cy - f->cap / 2 + 4, 4, 2, col);   /* the degree */
    return 38 + w + 12;
}

/* a small ring with a value under it */
static void face_ring(int cx, int cy, int r, float p, uint16_t col, const char *lbl)
{
    arc(cx, cy, r - 7, r, 0.0f, PI2, C_OFF);
    if ( p > 0.0f ) { arc(cx, cy, r - 7, r, 0.0f, PI2 * ((p > 1.0f) ? 1.0f : p), col); }
    text_c(&KF_CAPTION, cx, cy - KF_CAPTION.cap / 2, lbl, C_LIGHT);
}

/* "Big": the time large in the middle, three things under it */
static void draw_face_big(void)
{
    kk_clock_t c;
    char t[8] = "--:--";
    bool const ok = kk_clock(&c);
    if ( ok ) { (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min); }
    text_c(&KF_CLOCK, CX, 96, t, C_LIGHT);
    char d[24] = "";
    if ( ok ) { (void)snprintf(d, sizeof(d), "%s %d %s", DAY[c.wday], c.mday, MON[c.mon - 1]); }
    text_c(&KF_BODY, CX, 200, d, C_MED);
    {
        cal_ev_t const *const e = cal_next();
        if ( e )
        {
            char w[12], t[64];
            cal_when(e, w, sizeof(w));
            kk_clock_t c2;
            bool const tom = kk_clock(&c2) && (e->ymd != cal_today(&c2));
            char dn[12] = "";
            if ( tom )
            {
                int const yy = (int)(e->ymd / 10000), mm = (int)(e->ymd / 100 % 100), dd = (int)(e->ymd % 100);
                (void)snprintf(dn, sizeof(dn), "%s ", (e->ymd == cal_tomorrow(cal_today(&c2))) ? "tmrw" : DAY[cal_wday(yy, mm, dd)]);
            }
            (void)snprintf(t, sizeof(t), "%s%s %s", dn, w, e->t);
            fit_text(&KF_CAPTION, t, KK_W - 80);
            kk_round(CX - 6 - kf_width(&KF_CAPTION, t) / 2 - 12, 240, kf_width(&KF_CAPTION, t) + 36, 32, 14, C_ROW);
            kk_round(CX - kf_width(&KF_CAPTION, t) / 2 - 10, 250, 6, 12, 3, CAL_COL[e->c & 3]);
            text_c(&KF_CAPTION, CX + 4, 256 - KF_CAPTION.cap / 2, t, C_LIGHT);
        }
    }
    int const y = 340;
    if ( wx_ok )
    {
        int const w = 38 + kf_width(&KF_NAME, "00") + 12;
        (void)wx_draw(80 - w / 2, y, &KF_NAME, C_LIGHT);
    }
    else { text_c(&KF_CAPTION, 80, y - KF_CAPTION.cap / 2, "no weather", C_REST); }
    long const n = (st_day >= 0) ? today_steps() : 0;
    char sn[12];
    if ( n >= 10000 ) { (void)snprintf(sn, sizeof(sn), "%ldk", n / 1000); }
    else { (void)snprintf(sn, sizeof(sn), "%ld", n); }
    float const p = (set_goal > 0) ? (float)n / (float)set_goal : 0.0f;
    face_ring(CX, y, 44, p, (p >= 1.0f) ? C_OK : C_YEL, sn);
    char b[8];
    (void)snprintf(b, sizeof(b), "%d%%", (battery < 0) ? 0 : battery);
    face_ring(KK_W - 80, y, 44, (battery < 0) ? 0.0f : (float)battery / 100.0f, charging ? C_CHG : ((battery < 20) ? C_RED : C_OK), b);
    text_c(&KF_CAPTION, CX, y + 58, "steps", C_REST);
    if ( nnotes )
    {
        char nn[24];
        (void)snprintf(nn, sizeof(nn), "%d notification%s", nnotes, (1 == nnotes) ? "" : "s");
        text_c(&KF_CAPTION, CX, 452, nn, C_BLUE);
    }
}

/* "Rings": steps, battery and the minute's seconds round the time */
static void draw_face_rings(void)
{
    int const cy = 236;
    kk_clock_t c;
    bool const ok = kk_clock(&c);
    long const n = (st_day >= 0) ? today_steps() : 0;
    float const ps = (set_goal > 0) ? (float)n / (float)set_goal : 0.0f;
    arc(CX, cy, 172, 186, 0.0f, PI2, KK_RGB(0x33, 0x29, 0x0A));
    if ( ps > 0.0f ) { arc(CX, cy, 172, 186, 0.0f, PI2 * ((ps > 1.0f) ? 1.0f : ps), (ps >= 1.0f) ? C_OK : C_YEL); }
    arc(CX, cy, 152, 166, 0.0f, PI2, KK_RGB(0x0E, 0x2A, 0x16));
    if ( battery > 0 ) { arc(CX, cy, 152, 166, 0.0f, PI2 * (float)battery / 100.0f, charging ? C_CHG : C_OK); }
    float const sec = ok ? (float)c.sec / 60.0f : 0.0f;
    arc(CX, cy, 134, 144, 0.0f, PI2, KK_RGB(0x05, 0x22, 0x30));
    if ( sec > 0.0f ) { arc(CX, cy, 134, 144, 0.0f, PI2 * sec, C_BLUE); }
    char t[8] = "--:--";
    if ( ok ) { (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min); }
    text_c(&KF_CLOCK, CX, cy - KF_CLOCK.cap / 2 - 6, t, C_LIGHT);
    char d[24] = "";
    if ( ok ) { (void)snprintf(d, sizeof(d), "%s %d", DAY[c.wday], c.mday); }
    text_c(&KF_CAPTION, CX, cy + KF_CLOCK.cap / 2 + 8, d, C_MED);
    if ( wx_ok )
    {
        int const w = 38 + kf_width(&KF_CAPTION, "00") + 12;
        (void)wx_draw(CX - w / 2, cy - KF_CLOCK.cap / 2 - 40, &KF_CAPTION, C_LIGHT);
    }
    char sn[16];
    (void)snprintf(sn, sizeof(sn), "%ld steps", n);
    text_c(&KF_CAPTION, CX, 452, sn, C_REST);
}

static void draw_face(uint32_t now)
{
    if ( 1 == set_face % 3 ) { draw_face_big(); return; }
    if ( 2 == set_face % 3 ) { draw_face_rings(); return; }
    draw_face_kody(now);
}

static void draw_face_kody(uint32_t now)
{
    kody_draw(g_fb, KODY_Y, now);
    face_time(40, 78, C_LIGHT);
    int const dw = face_date(42, 172, C_MED);
    pile(338, 46, false);
    if ( linked ) { kk_disc(318, 56, 5, C_BLUE); }
    if ( imu_ok() && (st_day >= 0) )
    {
        long const n = today_steps();
        float const p = (set_goal > 0) ? (float)n / (float)set_goal : 0.0f;
        arc(362, 112, 20, 27, 0.0f, PI2, C_OFF);
        if ( p > 0.0f ) { arc(362, 112, 20, 27, 0.0f, PI2 * ((p > 1.0f) ? 1.0f : p), (p >= 1.0f) ? C_OK : C_YEL); }
        kf_icon(g_fb, &KI_HEART_40, 362, 112, (p >= 1.0f) ? C_OK : C_MED);
        char sn[12];
        if ( n >= 10000 ) { (void)snprintf(sn, sizeof(sn), "%ld.%ldk", n / 1000, (n % 1000) / 100); }
        else { (void)snprintf(sn, sizeof(sn), "%ld", n); }
        text_c(&KF_CAPTION, 362, 144, sn, C_MED);
    }
    int x = 42 + dw + 18;
    x += wx_draw(x - 6, 172 + KF_BODY.cap / 2, &KF_BODY, C_MED);
    if ( nnotes )
    {
        char n[6];
        (void)snprintf(n, sizeof(n), "%d", nnotes);
        kk_disc(x + 10, 172 + KF_BODY.cap / 2, 14, C_BLUE);
        text_c(&KF_CAPTION, x + 10, 172 + KF_BODY.cap / 2 - KF_CAPTION.cap / 2, n, C_SOFT);
        x += 36;
    }
    if ( timer_run )
    {
        char t[12];
        (void)snprintf(t, sizeof(t), "%d:%02d", timer_left / 60, timer_left % 60);
        text(&KF_BODY, x, 172, t, C_YEL);
    }
    /* now and then Kody looks up, at you or at the time */
    if ( 0U == kody_next_react ) { kody_next_react = now + 20000U; }
    else if ( now > kody_next_react )
    {
        kody_play((kk_rand(2) == 0) ? KODY_LOOKS : KODY_TIME);
        kody_next_react = now + 20000U + kk_rand(20000U);
    }
}

static void draw_rest(void)
{
    kk_clock_t c;
    bool const ok = kk_clock(&c);
    int const m = ok ? c.min : 0;
    int const ox = ((m % 5) - 2) * 4, oy = (((m / 5) % 5) - 2) * 4;   /* burn-in: a small walk */
    if ( docked )
    {
        uint16_t const warm = KK_RGB(0xB0, 0x6A, 0x10);
        char t[8] = "--:--";
        if ( ok ) { (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min); }
        text_c(&KF_CLOCK, CX + ox, 190 + oy, t, warm);
        char l[24];
        (void)snprintf(l, sizeof(l), "charging %d%%", (battery < 0) ? 0 : battery);
        text_c(&KF_BODY, CX + ox, 300 + oy, l, KK_RGB(0x60, 0x3A, 0x0A));
        return;
    }
    face_time(40 + ox, 78 + oy, C_REST);
    (void)face_date(42 + ox, 172 + oy, KK_RGB(0x3A, 0x38, 0x35));
    pile(338 + ox, 46 + oy, true);
    if ( nnotes ) { kk_disc(318 + ox, 56 + oy, 5, KK_RGB(0x02, 0x45, 0x61)); }
}

/* ---- the launcher's rail ---- */

/* The rail shows these; RC car, Craft and the Store's apps are in the Apps folder. */
static const int RAIL[] = { A_NOTIF, A_TIMER, A_CAL, A_MUSIC, A_FINDPH, A_ALARM, A_STEPS, A_SLEEP, A_HOME, A_FOLDER, A_STORE, A_FILES,
#if CONFIG_BT_ENABLED
                            A_PHONE,
#endif
                            A_BATT, A_WIFI, A_SET };
#define RAIL_N ((int)(sizeof(RAIL) / sizeof(RAIL[0])))
static int rail_pos(int a)
{
    for ( int p = 0; p < RAIL_N; ++p ) { if ( RAIL[p] == a ) { return p; } }
    return 0;
}
static int rail_max(void) { return (RAIL_N - 1) * ROW_H; }

static int row_mid(int i) { return FOCUS_Y + i * ROW_H - (int)lroundf(rail_u); }

/* Rows away from the focus line lean in along the glass and dim. */
static int row_lean(int mid)
{
    int const d = (mid > FOCUS_Y) ? (mid - FOCUS_Y) : (FOCUS_Y - mid);
    return (d * d) / 1400;
}

static void rail_row(int i, int mid, int dim256)
{
    int const x = 30 + row_lean(mid);
    uint16_t const disc = kk_mix(APPS[i].disc, KK_BLACK, dim256);
    kk_disc(x + 40, mid, 40, disc);
    kf_icon(g_fb, APPS[i].glyph, x + 40, mid, kk_mix(APPS[i].ink, disc, dim256));
    text(&KF_NAME, x + 104, mid - KF_NAME.cap / 2, APPS[i].name, kk_mix(C_LIGHT, KK_BLACK, dim256));
    if ( (A_NOTIF == i) && nnotes )
    {
        char b[6];
        (void)snprintf(b, sizeof(b), "%d", nnotes);
        int const bx = x + 104 + kf_width(&KF_NAME, APPS[i].name) + 26;
        kk_disc(bx, mid, 15, kk_mix(C_RED, KK_BLACK, dim256));
        text_c(&KF_CAPTION, bx, mid - KF_CAPTION.cap / 2, b, kk_mix(C_LIGHT, KK_BLACK, dim256));
    }
}

static void draw_apps(int dim_all)
{
    int const focus = (int)lroundf(rail_u / ROW_H);
    for ( int p = 0; p < RAIL_N; ++p )
    {
        int const i = RAIL[p];
        int const mid = row_mid(p);
        if ( (mid < -60) || (mid > KK_H + 60) ) { continue; }
        int const d = (mid > FOCUS_Y) ? (mid - FOCUS_Y) : (FOCUS_Y - mid);
        int dim = (d > ROW_H) ? ((d - ROW_H) * 256) / (2 * ROW_H) : 0;
        dim += dim_all;
        if ( dim > 230 ) { dim = 230; }
        if ( launching && (i == launch_app) ) { continue; }
        rail_row(i, mid, dim);
        if ( (p == focus) && !launching )
        {
            int const x = 30 + row_lean(mid);
            arc(x + 40, mid, 45, 49, 0.0f, PI2, C_LIGHT);   /* the focus ring */
        }
    }
    fade_rows(0, 60, true);
    fade_rows(KK_H - 60, KK_H, false);
}

static int rail_at(int y)
{
    for ( int p = 0; p < RAIL_N; ++p )
    {
        int const mid = row_mid(p);
        if ( (y >= mid - ROW_H / 2) && (y < mid + ROW_H / 2) ) { return RAIL[p]; }
    }
    return -1;
}

static void launch(int a)
{
    launching = true;
    sound_play(SND_CLICK);
    launch_t0 = kk_ms();
    launch_app = a;
    launch_from_y = row_mid(rail_pos(a));
}

/* The flight to the middle, the Ring that turns and closes green, the fade. */
static bool draw_launch(uint32_t now)
{
    uint32_t const t = now - launch_t0;
    float const f = (t >= L_FLIGHT) ? 1.0f : (float)t / (float)L_FLIGHT;
    float const e = 1.0f - (1.0f - f) * (1.0f - f);
    int const dim = (int)(e * 200.0f);
    draw_apps(dim);
    int const x0 = 30 + row_lean(launch_from_y) + 40;
    int const cx = x0 + (int)((float)(CX - x0) * e);
    int const cy = launch_from_y + (int)((float)(FOCUS_Y - launch_from_y) * e);
    int const r = 40 + (int)(48.0f * e);
    kk_disc(cx, cy, r, APPS[launch_app].disc);
    kf_icon(g_fb, APPS[launch_app].glyph, cx, cy, APPS[launch_app].ink);
    if ( t >= L_FLIGHT )
    {
        uint32_t const s = t - L_FLIGHT;
        int const rin = r + 15, rout = r + 25;
        if ( s < L_SPIN )
        {
            float const p = (float)s / (float)L_SPIN;
            arc(cx, cy, rin, rout, PI2 * p * 1.5f, PI2 * p, C_OK);
        }
        else { arc(cx, cy, rin, rout, 0.0f, PI2, C_OK); }
    }
    uint32_t const fade_at = L_FLIGHT + L_SPIN + L_HOLD;
    if ( t >= fade_at )
    {
        int const k = (int)(((t - fade_at) * 256U) / L_FADE);
        if ( k >= 256 ) { return true; }
        for ( int y = 0; y < KK_H; ++y )
        {
            uint16_t *const row = &g_fb[y * KK_W];
            for ( int x = 0; x < KK_W; ++x ) { row[x] = kk_mix(row[x], KK_BLACK, k); }
        }
    }
    return false;
}

/* ---- the control centre ---- */

#define CC_D 100
static const int CC_X[3] = { 35, 155, 275 };
static const int CC_Y[2] = { 44, 160 };
#define LV_X 18
#define LV_W 374
#define LV_H 84
#define LV1_Y 276
#define LV2_Y 376

static void cc_circle(int col, int row, uint16_t bg)
{
    kk_disc(CC_X[col] + CC_D / 2, CC_Y[row] + CC_D / 2, CC_D / 2, bg);
}

static void level(int y, int pct, const kf_icon_t *gl, const char *fig)
{
    kk_round(LV_X, y, LV_W, LV_H, LV_H / 2, C_OFF);
    int const w = LV_H + ((LV_W - LV_H) * pct) / 100;
    kk_round(LV_X, y, w, LV_H, LV_H / 2, C_LIGHT);
    kf_icon(g_fb, gl, LV_X + 16 + 26, y + LV_H / 2, C_SOFT);
    int const fw = kf_width(&KF_FIGURE, fig);
    int const fx = LV_X + w - fw - 26;
    if ( fx > LV_X + 84 ) { text(&KF_FIGURE, fx, y + (LV_H - KF_FIGURE.cap) / 2, fig, C_SOFT); }
    else { text(&KF_FIGURE, LV_X + w + 18, y + (LV_H - KF_FIGURE.cap) / 2, fig, C_LIGHT); }
}

static void draw_cc(uint32_t now)
{
    net_status_t st;
    net_status(&st);
    /* Wi-Fi, the hub, more */
    cc_circle(0, 0, st.wifi_up ? C_BLUE : C_OFF);
    kf_icon(g_fb, &KI_WIFI_46, CC_X[0] + 50, CC_Y[0] + 50, st.wifi_up ? C_SOFT : C_LIGHT);
    ble_status_t bs;
    ble_status(&bs);
    cc_circle(1, 0, bs.secure ? C_BLUE : C_OFF);
    kf_icon(g_fb, &KI_BT_46, CC_X[1] + 50, CC_Y[0] + 50, bs.secure ? C_SOFT : C_LIGHT);
    cc_circle(2, 0, C_OFF);
    kf_icon(g_fb, &KI_MORE_46, CC_X[2] + 50, CC_Y[0] + 50, C_LIGHT);
    /* the battery's liquid, sharing (not yet), turn off */
    int const bx = CC_X[0] + 50, by = CC_Y[1] + 50;
    cc_circle(0, 1, C_OFF);
    if ( battery >= 0 ) { liquid(bx, by, 50, battery, charging ? C_CHG : (battery < 20 ? C_RED : C_OK)); }
    char b[8];
    (void)snprintf(b, sizeof(b), "%d%%", (battery < 0) ? 0 : battery);
    text_c(&KF_NAME, bx, by - KF_NAME.cap / 2, b, (battery > 55) ? C_SOFT : C_LIGHT);
    cc_circle(1, 1, sound_on() ? C_BLUE : C_OFF);
    kf_icon(g_fb, &KI_NOTE_40, CC_X[1] + 50, CC_Y[1] + 50, sound_on() ? C_SOFT : C_LIGHT);
    bool const armed = off_armed_until > now;
    cc_circle(2, 1, armed ? C_RED : C_OFF);
    arc(CC_X[2] + 50, CC_Y[1] + 50, 46, 50, 0.0f, PI2, C_RED);
    kf_icon(g_fb, &KI_POWER_46, CC_X[2] + 50, CC_Y[1] + 50, armed ? C_SOFT : C_RED);

    char f[12];
    (void)snprintf(f, sizeof(f), "%d%%", set_bright);
    level(LV1_Y, (set_bright - 10) * 100 / 90, &KI_SUN3_52, f);
    if ( REST_S[set_rest] ) { (void)snprintf(f, sizeof(f), "%d s", REST_S[set_rest]); }
    else { (void)snprintf(f, sizeof(f), "-"); }
    level(LV2_Y, (set_rest * 100) / 4, &KI_CLOCK_40, f);
    if ( armed ) { text_c(&KF_CAPTION, CX, 466, "tap again to turn off", C_RED); }
    else { kk_round(CX - 28, 478, 56, 5, 2, C_REST); }
}

static void cc_level_drag(int x)
{
    int p = ((x - LV_X - LV_H / 2) * 100) / (LV_W - LV_H);
    if ( p < 0 ) { p = 0; }
    if ( p > 100 ) { p = 100; }
    if ( 1 == drag_level )
    {
        set_bright = 10 + (p * 90) / 100;
        apply_bright();
    }
    else if ( 2 == drag_level )
    {
        set_rest = (p * 4 + 50) / 100;
    }
}

static void tap_cc(int x, int y, uint32_t now)
{
    for ( int r = 0; r < 2; ++r )
    {
        for ( int c = 0; c < 3; ++c )
        {
            int const dx = x - (CC_X[c] + 50), dy = y - (CC_Y[r] + 50);
            if ( (dx * dx + dy * dy) > 54 * 54 ) { continue; }
            if ( (0 == r) && (0 == c) ) { open_app(A_WIFI); }
#if CONFIG_BT_ENABLED
            else if ( (0 == r) && (1 == c) ) { open_app(A_PHONE); }
#endif
            else if ( (0 == r) && (2 == c) ) { open_app(A_SET); }
            else if ( (1 == r) && (1 == c) )
            {
                sound_set_on(!sound_on());
                save_settings();
                sound_play(SND_NOTE);
            }
            else if ( (1 == r) && (2 == c) )
            {
                if ( off_armed_until > now ) { board_power_off(); }
                else { off_armed_until = now + 3000; }
            }
            return;
        }
    }
}

/* ---- apps ---- */

/* @p s word-wrapped in lines of at most @p w pixels, from @p y down; at most @p maxl lines */
static int wrap_text(const kf_font_t *f, int x, int y, int w, int lh, int maxl, const char *s, uint16_t col)
{
    int lines = 0;
    while ( *s && (lines < maxl) )
    {
        while ( ' ' == *s ) { ++s; }
        char ln[NLEN];
        int n = 0, cut = -1;
        while ( s[n] && (n < (int)sizeof(ln) - 1) )
        {
            ln[n] = s[n];
            ln[n + 1] = '\0';
            if ( kf_width(f, ln) > w ) { break; }
            if ( ' ' == s[n] ) { cut = n; }
            ++n;
        }
        if ( s[n] && (cut > 0) ) { n = cut; }   /* break at the last space that fits */
        if ( 0 == n ) { n = 1; }
        ln[n] = '\0';
        if ( (lines == maxl - 1) && s[n] ) { char t[NLEN + 4]; fit(f, s, w, t, sizeof(t)); text(f, x, y, t, col); return lines + 1; }
        text(f, x, y, ln, col);
        y += lh;
        ++lines;
        s += n;
    }
    return lines;
}

static void draw_note_open(void)
{
    int const i = note_open;
    uint32_t const ago = (kk_ms() - note_at[i]) / 60000U;
    char a[48];
    if ( ago ) { (void)snprintf(a, sizeof(a), "%s%s%lu min ago", note_app[i], note_app[i][0] ? "  -  " : "", (unsigned long)ago); }
    else { (void)snprintf(a, sizeof(a), "%s%snow", note_app[i], note_app[i][0] ? "  -  " : ""); }
    text_c(&KF_CAPTION, CX, 76, a, C_MED);
    kk_round(18, 104, KK_W - 36, 280, 26, C_ROW);
    (void)wrap_text(&KF_BODY, 38, 122, KK_W - 76, KF_BODY.cap + 14, 9, notes[i], C_LIGHT);
    pill(26, 404, 170, 64, "Delete", C_RED, C_LIGHT);
    pill(KK_W - 196, 404, 170, 64, "Back", C_OFF, C_LIGHT);
    back_bar("Notification");
}

static void draw_notif(void)
{
    if ( (note_open >= 0) && (note_open < nnotes) ) { draw_note_open(); return; }
    note_open = -1;
    if ( 0 == nnotes )
    {
        kf_icon(g_fb, &KI_LIST_40, CX, 210, C_REST);
        text_c(&KF_BODY, CX, 250, "No notifications", C_REST);
    }
    int y = 90;
    for ( int i = nnotes - 1; (i >= 0) && (y < 360); --i, y += 96 )
    {
        kk_round(18, y, KK_W - 36, 88, 30, C_ROW);
        kk_disc(18 + 46, y + 44, 30, C_BLUE);
        kf_icon(g_fb, &KI_LIST_40, 18 + 46, y + 44, C_SOFT);
        char line[NLEN];
        fit(&KF_NAME, notes[i], KK_W - 36 - 104 - 20, line, sizeof(line));
        text(&KF_NAME, 18 + 92, y + 18, line, C_LIGHT);
        uint32_t const ago = (kk_ms() - note_at[i]) / 60000U;
        char a[40];
        if ( ago ) { (void)snprintf(a, sizeof(a), "%s%s%lu min ago", note_app[i], note_app[i][0] ? "  " : "", (unsigned long)ago); }
        else { (void)snprintf(a, sizeof(a), "%s%snow", note_app[i], note_app[i][0] ? "  " : ""); }
        text(&KF_CAPTION, 18 + 92, y + 56, a, C_MED);
    }
    if ( nnotes ) { pill(CX - 120, 404, 240, 64, "Clear all", C_OFF, C_LIGHT); }
    back_bar("Notifications");
}

static void note_delete(int i)
{
#if CONFIG_BT_ENABLED
    if ( note_uid[i] ) { ble_notif_action(note_uid[i], false); }   /* gone on the iPhone too */
#endif
    del_note(i);
}

static void tap_notif(int x, int y)
{
    if ( (note_open >= 0) && (note_open < nnotes) )
    {
        if ( hit(x, y, 26, 404, 170, 64) ) { note_delete(note_open); note_open = -1; sound_play(SND_CLICK); }
        else if ( hit(x, y, KK_W - 196, 404, 170, 64) ) { note_open = -1; }
        return;
    }
    if ( nnotes && hit(x, y, CX - 120, 404, 240, 64) ) { nnotes = 0; return; }
    int yy = 90;
    for ( int i = nnotes - 1; (i >= 0) && (yy < 360); --i, yy += 96 )
    {
        if ( hit(x, y, 18, yy, KK_W - 36, 88) ) { note_open = i; sound_play(SND_CLICK); return; }
    }
}

#define TM_CY 230
#define TM_R 165
static void draw_timer(void)
{
    arc(CX, TM_CY, TM_R - 12, TM_R, 0.0f, PI2, C_OFF);
    if ( timer_set > 0 )
    {
        float const p = (float)timer_left / (float)timer_set;
        if ( p > 0.0f ) { arc(CX, TM_CY, TM_R - 12, TM_R, 0.0f, PI2 * p, timer_run ? C_YEL : C_REST); }
    }
    char t[8];
    (void)snprintf(t, sizeof(t), "%d:%02d", timer_left / 60, timer_left % 60);
    text_c(&KF_CLOCK, CX, TM_CY - KF_CLOCK.cap / 2 - 10, t, (alarm_until > kk_ms()) ? C_RED : C_LIGHT);
    text_c(&KF_CAPTION, CX, TM_CY + 52, timer_run ? "running" : "paused", C_MED);
    pill(26, 400, 108, 64, "+1", C_OFF, C_LIGHT);
    pill(CX - 64, 400, 128, 64, timer_run ? "Pause" : "Start", timer_run ? C_YEL : C_OK, C_SOFT);
    pill(KK_W - 134, 400, 108, 64, "Reset", C_OFF, C_LIGHT);
    back_bar("Timer");
}

static void tap_timer(int x, int y)
{
    if ( hit(x, y, 26, 400, 108, 64) ) { timer_add(60); }
    else if ( hit(x, y, CX - 64, 400, 128, 64) ) { timer_toggle(); }
    else if ( hit(x, y, KK_W - 134, 400, 108, 64) ) { timer_run = false; timer_left = timer_set = 300; }
}

#define HM_Y0 88
#define HM_ROW 92
static void draw_home(void)
{
    for ( int i = 0; i < 4; ++i )
    {
        int const y = HM_Y0 + i * HM_ROW;
        bool const on = (i < 3) && home_on[i];
        kk_round(18, y, KK_W - 36, HM_ROW - 8, 30, C_ROW);
        kk_disc(18 + 46, y + 42, 30, on ? C_YEL : C_OFF);
        kf_icon(g_fb, (3 == i) ? &KI_POWER_46 : &KI_HOME_40, 18 + 46, y + 42, on ? C_SOFT : C_LIGHT);
        text(&KF_NAME, 18 + 92, y + 42 - KF_NAME.cap / 2, ROOM[i], C_LIGHT);
        if ( i < 3 )
        {
            /* a switch: the track and its knob */
            int const sx = KK_W - 18 - 22 - 72, sy = y + 42 - 20;
            kk_round(sx, sy, 72, 40, 20, on ? C_OK : C_OFF);
            kk_disc(on ? (sx + 72 - 20) : (sx + 20), sy + 20, 16, C_LIGHT);
        }
    }
    back_bar("Home");
}

static void tap_home(int x, int y)
{
    for ( int i = 0; i < 4; ++i )
    {
        if ( hit(x, y, 18, HM_Y0 + i * HM_ROW, KK_W - 36, HM_ROW - 8) ) { home_toggle(i); return; }
    }
}

static void info_row(int y, const char *k, const char *v, uint16_t vc)
{
    kk_round(18, y, KK_W - 36, 52, 22, C_ROW);
    text(&KF_BODY, 40, y + (52 - KF_BODY.cap) / 2, k, C_MED);
    text(&KF_NAME, KK_W - 40 - kf_width(&KF_NAME, v), y + (52 - KF_NAME.cap) / 2, v, vc);
}

static void mb(char *out, size_t cap, uint64_t bytes)
{
    if ( bytes >= (1024ULL * 1024ULL * 1024ULL) ) { (void)snprintf(out, cap, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0)); }
    else { (void)snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0)); }
}

static void draw_info(void)
{
    net_status_t st;
    net_status(&st);
    storage_info_t si;
    storage_info(&si);
    char v[40], a[16];
    int y = 80;
    {
        char vk[40];
        (void)snprintf(vk, sizeof(vk), "%s  Kody %s %d", KW_VERSION, kody_status(), kody_worlds());
        info_row(y, "Version", vk, kody_ready() ? C_LIGHT : C_RED);
    }
    y += 56;
    if ( battery >= 0 ) { (void)snprintf(v, sizeof(v), "%d%%%s", battery, charging ? " charging" : ""); }
    else { (void)snprintf(v, sizeof(v), "-"); }
    info_row(y, "Battery", v, charging ? C_CHG : C_LIGHT);
    y += 56;
    char ss[20];
    fit(&KF_NAME, st.wifi_up ? st.ssid : "off", 200, ss, sizeof(ss));
    info_row(y, "Wi-Fi", ss, st.wifi_up ? C_OK : C_MED);
    y += 56;
    if ( st.pair[0] )
    {
        char pc[16];
        (void)snprintf(pc, sizeof(pc), "code %.3s %.3s", st.pair, st.pair + 3);
        info_row(y, "Hub", pc, C_YEL);
    }
    else
    {
        info_row(y, "Hub", st.hub_up ? "connected" : st.key_set ? "not connected" : "pairing...", st.hub_up ? C_OK : C_MED);
    }
    y += 56;
    if ( si.flash_mb ) { (void)snprintf(v, sizeof(v), "%u MB chip", (unsigned)si.flash_mb); }
    else { (void)snprintf(v, sizeof(v), "?"); }
    info_row(y, "Flash", v, (si.flash_mb > si.flash_cfg_mb) ? C_YEL : C_LIGHT);
    y += 56;
    if ( si.data_ok ) { mb(a, sizeof(a), si.data_free); (void)snprintf(v, sizeof(v), "%s free", a); }
    else { (void)snprintf(v, sizeof(v), "-"); }
    info_row(y, "Storage", v, C_LIGHT);
    y += 56;
    if ( si.sd_ok ) { mb(a, sizeof(a), si.sd_free); (void)snprintf(v, sizeof(v), "%s free", a); }
    else { fit(&KF_NAME, storage_sd_status(), 200, v, sizeof(v)); }
    info_row(y, "microSD", v, si.sd_ok ? C_OK : C_MED);
    if ( st.err[0] ) { text_c(&KF_CAPTION, CX, 474, st.err, C_RED); }
    back_bar("Info");
}

/* ---- the iPhone ---- */

static bool ble_asking(void)
{
    ble_status_t b;
    ble_status(&b);
    return b.numcmp_pending || b.passkey_show;
}

#if CONFIG_BT_ENABLED
static void draw_phone(void)
{
    ble_status_t b;
    ble_status(&b);
    kk_disc(CX, 128, 44, b.secure ? C_BLUE : C_OFF);
    kf_icon(g_fb, &KI_BT_46, CX, 128, b.secure ? C_SOFT : C_LIGHT);
    char l[48];
    const char *st;
    uint16_t col = C_MED;
    if ( !b.ready ) { st = "Bluetooth off"; col = C_RED; }
    else if ( b.secure ) { st = b.peer[0] ? b.peer : "Connected"; col = C_OK; }
    else if ( b.connected ) { st = "Pairing..."; col = C_YEL; }
    else if ( b.bonded_any ) { st = "Waiting for the iPhone"; }
    else { st = "Not paired"; }
    fit(&KF_NAME, st, KK_W - 60, l, sizeof(l));
    text_c(&KF_NAME, CX, 188, l, col);
    if ( !b.bonded_any && !b.connected )
    {
        text_c(&KF_CAPTION, CX, 236, "On the iPhone: Settings >", C_MED);
        text_c(&KF_CAPTION, CX, 266, "Bluetooth > Kode Watch", C_MED);
        back_bar("iPhone");
        return;
    }
    if ( b.secure )
    {
        char sv[48];
        (void)snprintf(sv, sizeof(sv), "%s%s%s", b.ancs ? "Notifications  " : "", b.ams ? "Music  " : "", b.cts ? "Time" : "");
        text_c(&KF_CAPTION, CX, 222, sv[0] ? sv : "looking for services...", C_REST);
    }
    if ( b.secure && b.ams )
    {
        kk_round(18, 256, KK_W - 36, 92, 30, C_ROW);
        fit(&KF_NAME, b.title[0] ? b.title : "Nothing playing", KK_W - 76, l, sizeof(l));
        text_c(&KF_NAME, CX, 272, l, C_LIGHT);
        fit(&KF_CAPTION, b.artist, KK_W - 76, l, sizeof(l));
        text_c(&KF_CAPTION, CX, 310, l, C_MED);
        pill(26, 362, 108, 64, "<<", C_OFF, C_LIGHT);
        pill(CX - 64, 362, 128, 64, (1 == b.playing) ? "Pause" : "Play", (1 == b.playing) ? C_YEL : C_OK, C_SOFT);
        pill(KK_W - 134, 362, 108, 64, ">>", C_OFF, C_LIGHT);
    }
    pill(CX - 90, 436, 180, 52, "Forget", C_OFF, C_RED);
    back_bar("iPhone");
}

static uint32_t forget_armed;
static void tap_phone(int x, int y, uint32_t now)
{
    ble_status_t b;
    ble_status(&b);
    if ( b.secure && b.ams )
    {
        if ( hit(x, y, 26, 362, 108, 64) ) { ble_music(BLE_MUSIC_PREV); return; }
        if ( hit(x, y, CX - 64, 362, 128, 64) ) { ble_music(BLE_MUSIC_TOGGLE); return; }
        if ( hit(x, y, KK_W - 134, 362, 108, 64) ) { ble_music(BLE_MUSIC_NEXT); return; }
    }
    if ( (b.bonded_any || b.connected) && hit(x, y, CX - 90, 436, 180, 52) )
    {
        if ( forget_armed > now ) { forget_armed = 0; ble_forget(); }
        else { forget_armed = now + 3000; add_note("Tap Forget again to unpair"); }
    }
}

#endif

/* a modal over everything: true when it drew */
static bool draw_modal(uint32_t now)
{
    ble_status_t b;
    ble_status(&b);
    if ( b.numcmp_pending || b.passkey_show )
    {
        char code[12];
        (void)snprintf(code, sizeof(code), "%06lu", (unsigned long)b.numcmp);
        kk_disc(CX, 110, 44, C_BLUE);
        kf_icon(g_fb, &KI_BT_46, CX, 110, C_SOFT);
        text_c(&KF_TITLE, CX, 172, b.passkey_show ? "Type on the iPhone" : "Pair with iPhone?", C_LIGHT);
        text_c(&KF_FIGURE, CX, 236, code, C_YEL);
        if ( b.passkey_show ) { pill(CX - 90, 380, 180, 64, "OK", C_OFF, C_LIGHT); }
        else
        {
            text_c(&KF_CAPTION, CX, 312, "same code on the iPhone?", C_MED);
            pill(26, 380, 170, 64, "No", C_OFF, C_LIGHT);
            pill(KK_W - 196, 380, 170, 64, "Pair", C_OK, C_SOFT);
        }
        return true;
    }
    if ( call_uid )
    {
        if ( (now - call_ring) > 1500U ) { call_ring = now; sound_play(SND_NOTE); kk_buzz(400); }
        text_c(&KF_CAPTION, CX, 120, "Incoming call", C_MED);
        char l[48];
        fit(&KF_TITLE, call_name, KK_W - 40, l, sizeof(l));
        text_c(&KF_TITLE, CX, 170, l, C_LIGHT);
        kk_disc(110, 360, 56, C_RED);
        text_c(&KF_NAME, 110, 360 - KF_NAME.cap / 2, "No", C_LIGHT);
        kk_disc(KK_W - 110, 360, 56, C_OK);
        text_c(&KF_NAME, KK_W - 110, 360 - KF_NAME.cap / 2, "Yes", C_SOFT);
        text_c(&KF_CAPTION, CX, 446, "answer on the iPhone", C_REST);
        return true;
    }
    return false;
}

static bool tap_modal(int x, int y)
{
    ble_status_t b;
    ble_status(&b);
    if ( b.passkey_show )
    {
        if ( hit(x, y, CX - 90, 380, 180, 64) ) { ble_pair_answer(true); }
        return true;
    }
    if ( b.numcmp_pending )
    {
        if ( hit(x, y, 26, 380, 170, 64) ) { ble_pair_answer(false); }
        else if ( hit(x, y, KK_W - 196, 380, 170, 64) ) { ble_pair_answer(true); }
        return true;
    }
    if ( call_uid )
    {
        int const dl = (x - 110) * (x - 110) + (y - 360) * (y - 360);
        int const dr = (x - (KK_W - 110)) * (x - (KK_W - 110)) + (y - 360) * (y - 360);
        if ( dl < 66 * 66 ) { ble_notif_action(call_uid, false); call_uid = 0; }
        else if ( dr < 66 * 66 ) { ble_notif_action(call_uid, true); call_uid = 0; }
        return true;
    }
    return false;
}

/* ---- Files: the microSD and the flash's /data ---- */

#define FL_MAX 200
#define FL_Y0 88
#define FL_RH 62
#define FL_ROWS 5
EXT_RAM_BSS_ATTR static fs_ent_t fl_e[FL_MAX];
static char fl_dir[FS_PATH];   /* "" = the two roots */
static int fl_n = -2;          /* -2 = to read, -1 = could not */
static int fl_off, fl_sel = -1;
static uint32_t fl_del_until;

static void fl_open(const char *dir)
{
    char d[FS_PATH];
    (void)snprintf(d, sizeof(d), "%s", dir);   /* dir may be fl_dir itself */
    memcpy(fl_dir, d, sizeof(fl_dir));
    fl_n = -2;
    fl_off = 0;
    fl_sel = -1;
}

static void fl_load(void)
{
    if ( -2 != fl_n ) { return; }
    if ( '\0' == fl_dir[0] )
    {
        storage_info_t si;
        storage_info(&si);
        fl_n = 0;
        if ( si.sd_ok ) { (void)snprintf(fl_e[fl_n].name, sizeof(fl_e[0].name), "sdcard"); fl_e[fl_n].dir = true; fl_e[fl_n].size = 0; ++fl_n; }
        if ( si.data_ok ) { (void)snprintf(fl_e[fl_n].name, sizeof(fl_e[0].name), "data"); fl_e[fl_n].dir = true; fl_e[fl_n].size = 0; ++fl_n; }
        return;
    }
    fl_n = fs_list(fl_dir, fl_e, FL_MAX);
}

static void fl_size(char *out, size_t cap, uint32_t b)
{
    if ( b < 1024U ) { (void)snprintf(out, cap, "%lu B", (unsigned long)b); }
    else if ( b < 1048576U ) { (void)snprintf(out, cap, "%lu KB", (unsigned long)(b / 1024U)); }
    else { (void)snprintf(out, cap, "%lu.%lu MB", (unsigned long)(b / 1048576U), (unsigned long)((b % 1048576U) * 10U / 1048576U)); }
}

/* the rows: ".." first when inside a folder */
static int fl_rows(void) { return ((fl_n > 0) ? fl_n : 0) + (fl_dir[0] ? 1 : 0); }

static void folder_glyph(int cx, int cy, uint16_t c)
{
    kk_round(cx - 15, cy - 12, 13, 6, 2, c);
    kk_round(cx - 15, cy - 8, 30, 21, 4, c);
}

static void draw_files(uint32_t now)
{
    fl_load();
    char l[64];
    const char *const shown = fl_dir[0] ? (0 == strncmp(fl_dir, "/sdcard", 7) ? "microSD" : "Flash") : "Storage";
    if ( fl_dir[0] && strchr(fl_dir + 1, '/') )
    {
        const char *const last = strrchr(fl_dir, '/') + 1;
        (void)snprintf(l, sizeof(l), "%s / %s", shown, last);
    }
    else { (void)snprintf(l, sizeof(l), "%s", shown); }
    char t[64];
    fit(&KF_CAPTION, l, KK_W - 60, t, sizeof(t));
    text_c(&KF_CAPTION, CX, BAR_FOOT + 2, t, C_MED);
    if ( -1 == fl_n ) { text_c(&KF_BODY, CX, 220, "Cannot read this folder", C_RED); back_bar("Files"); return; }
    int const rows = fl_rows();
    if ( 0 == rows ) { text_c(&KF_BODY, CX, 220, "No storage", C_REST); back_bar("Files"); return; }
    if ( fl_off > rows - FL_ROWS ) { fl_off = (rows > FL_ROWS) ? rows - FL_ROWS : 0; }
    for ( int r = 0; (r < FL_ROWS) && (fl_off + r < rows); ++r )
    {
        int const k = fl_off + r;
        int const y = FL_Y0 + 22 + r * FL_RH;
        bool const up = fl_dir[0] && (0 == k);
        int const i = k - (fl_dir[0] ? 1 : 0);
        bool const sel = !up && (i == fl_sel);
        kk_round(18, y, KK_W - 36, FL_RH - 8, 22, sel ? C_OFF : C_ROW);
        bool const dir = up || fl_e[i].dir;
        if ( dir ) { folder_glyph(18 + 34, y + (FL_RH - 8) / 2, up ? C_MED : C_YEL); }
        else { kk_disc(18 + 34, y + (FL_RH - 8) / 2, 14, C_BLUE); }
        const char *const nm = up ? ".. back" : (('\0' == fl_dir[0]) ? ((0 == strcmp(fl_e[i].name, "sdcard")) ? "microSD" : "Flash (/data)") : fl_e[i].name);
        char nl[64];
        fit(&KF_NAME, nm, dir ? KK_W - 130 : KK_W - 210, nl, sizeof(nl));
        text(&KF_NAME, 18 + 62, y + (FL_RH - 8 - KF_NAME.cap) / 2, nl, C_LIGHT);
        if ( !dir )
        {
            char sz[16];
            fl_size(sz, sizeof(sz), fl_e[i].size);
            int const w = kf_width(&KF_CAPTION, sz);
            text(&KF_CAPTION, KK_W - 36 - w, y + (FL_RH - 8 - KF_CAPTION.cap) / 2, sz, C_MED);
        }
    }
    if ( rows > FL_ROWS )
    {
        int const th = (FL_ROWS * FL_RH * FL_ROWS) / rows;
        int const ty = FL_Y0 + 22 + ((FL_ROWS * FL_RH - th) * fl_off) / (rows - FL_ROWS);
        kk_round(KK_W - 12, ty, 5, th, 2, C_REST);
    }
    if ( (fl_sel >= 0) && (fl_sel < fl_n) && !fl_e[fl_sel].dir )
    {
        bool const armed = fl_del_until > now;
        pill(CX - 110, 428, 220, 58, armed ? "Tap to delete" : "Delete", armed ? C_RED : C_OFF, armed ? C_LIGHT : C_RED);
    }
    back_bar("Files");
}

static void tap_files(int x, int y, uint32_t now)
{
    if ( (fl_sel >= 0) && (fl_sel < fl_n) && !fl_e[fl_sel].dir && hit(x, y, CX - 110, 428, 220, 58) )
    {
        if ( fl_del_until > now )
        {
            char p[FS_PATH + 64];
            (void)snprintf(p, sizeof(p), "%s/%s", fl_dir, fl_e[fl_sel].name);
            if ( !fs_remove(p) ) { add_note("Could not delete the file"); }
            fl_del_until = 0;
            int const keep = fl_off;
            fl_open(fl_dir);
            fl_off = keep;
        }
        else { fl_del_until = now + 3000; }
        return;
    }
    int const rows = fl_rows();
    for ( int r = 0; (r < FL_ROWS) && (fl_off + r < rows); ++r )
    {
        int const y0 = FL_Y0 + 22 + r * FL_RH;
        if ( !hit(x, y, 18, y0, KK_W - 36, FL_RH - 8) ) { continue; }
        int const k = fl_off + r;
        if ( fl_dir[0] && (0 == k) )
        {
            /* up one: the root of a volume goes back to the two roots */
            char *const s = strrchr(fl_dir, '/');
            if ( s && (s != fl_dir) ) { *s = '\0'; fl_open(fl_dir); }
            else { fl_open(""); }
            return;
        }
        int const i = k - (fl_dir[0] ? 1 : 0);
        if ( fl_e[i].dir )
        {
            char p[FS_PATH];
            if ( '\0' == fl_dir[0] ) { (void)snprintf(p, sizeof(p), "/%s", fl_e[i].name); }
            else { (void)snprintf(p, sizeof(p), "%s/%.60s", fl_dir, fl_e[i].name); }
            if ( fs_path_ok(p) ) { fl_open(p); }
        }
        else { fl_sel = (fl_sel == i) ? -1 : i; fl_del_until = 0; }
        return;
    }
}

/* ---- the Apps folder: RC car, Craft, and the Store's installed apps ---- */

#define FO_COLS 2
#define FO_ROWS 3
#define FO_W 170
#define FO_H 112
#define FO_X0 ((KK_W - FO_COLS * FO_W - 14) / 2)
#define FO_Y0 96
typedef struct { int os_app; int store; } fo_item_t;   /* one of the two; -1 when not */
EXT_RAM_BSS_ATTR static apps_item_t fo_cat[APPS_MAX];

static int fo_items(fo_item_t *out, int max)
{
    int n = 0;
    out[n++] = (fo_item_t){ A_RC, -1 };
    out[n++] = (fo_item_t){ A_CRAFT, -1 };
    int const c = apps_list(fo_cat, APPS_MAX);
    for ( int i = 0; (i < c) && (n < max); ++i ) { if ( fo_cat[i].installed ) { out[n++] = (fo_item_t){ -1, i }; } }
    return n;
}

static void draw_folder(void)
{
    fo_item_t it[APPS_MAX + 2];
    int const n = fo_items(it, APPS_MAX + 2);
    int const pages = (n + FO_COLS * FO_ROWS - 1) / (FO_COLS * FO_ROWS);
    if ( fo_off >= pages ) { fo_off = pages - 1; }
    if ( fo_off < 0 ) { fo_off = 0; }
    for ( int k = 0; k < FO_COLS * FO_ROWS; ++k )
    {
        int const i = fo_off * FO_COLS * FO_ROWS + k;
        if ( i >= n ) { break; }
        int const x = FO_X0 + (k % FO_COLS) * (FO_W + 14), y = FO_Y0 + (k / FO_COLS) * (FO_H + 12);
        kk_round(x, y, FO_W, FO_H, 28, C_ROW);
        int const cx = x + FO_W / 2, cy = y + 40;
        const char *name;
        if ( it[i].os_app >= 0 )
        {
            const app_t *const a = &APPS[it[i].os_app];
            kk_disc(cx, cy, 28, a->disc);
            kf_icon(g_fb, a->glyph, cx, cy, a->ink);
            name = a->name;
        }
        else
        {
            const apps_item_t *const a = &fo_cat[it[i].store];
            kk_disc(cx, cy, 28, a->color);
            char l[2] = { a->name[0], '\0' };
            text_c(&KF_NAME, cx, cy - KF_NAME.cap / 2, l, C_SOFT);
            name = a->name;
        }
        char nl[32];
        fit(&KF_CAPTION, name, FO_W - 16, nl, sizeof(nl));
        text_c(&KF_CAPTION, cx, y + 80, nl, C_LIGHT);
    }
    if ( pages > 1 )
    {
        for ( int p = 0; p < pages; ++p ) { kk_disc(CX - (pages - 1) * 9 + p * 18, 470, 4, (p == fo_off) ? C_LIGHT : C_REST); }
    }
    else if ( n <= 2 ) { text_c(&KF_CAPTION, CX, 360, "Apps from the Store land here", C_REST); }
    back_bar("Apps");
}

static void tap_folder(int x, int y)
{
    fo_item_t it[APPS_MAX + 2];
    int const n = fo_items(it, APPS_MAX + 2);
    for ( int k = 0; k < FO_COLS * FO_ROWS; ++k )
    {
        int const i = fo_off * FO_COLS * FO_ROWS + k;
        if ( i >= n ) { break; }
        int const tx = FO_X0 + (k % FO_COLS) * (FO_W + 14), ty = FO_Y0 + (k / FO_COLS) * (FO_H + 12);
        if ( !hit(x, y, tx, ty, FO_W, FO_H) ) { continue; }
        sound_play(SND_CLICK);
        if ( it[i].os_app >= 0 ) { open_app(it[i].os_app); }
        else if ( !apps_open(fo_cat[it[i].store].id) ) { add_note("Could not open the app"); }
        return;
    }
}

/* "os open <name>": an OS app by its name (the hub's /update, and tests) */
static void os_open_named(const char *nm)
{
    for ( int a = 0; a < A_N; ++a )
    {
        if ( 0 == strcasecmp(nm, APPS[a].name) ) { open_app(a); view = V_APP; wake(); return; }
    }
    if ( 0 == strcasecmp(nm, "face") ) { view = V_FACE; wake(); }
    else if ( 0 == strcasecmp(nm, "control") ) { view = V_CC; wake(); }
}

/* ---- Settings ---- */

#define SE_Y0 80
#define SE_RH 46
#define FACES 3
static const char *const FACE_NAME[FACES] = { "Kody", "Big", "Rings" };
enum { SE_BRIGHT, SE_REST, SE_AOD, SE_SOUND, SE_RAISE, SE_TAPW, SE_GOAL, SE_FACE, SE_ABOUT, SE_N };

static void se_value(int r, char *v, size_t cap, uint16_t *col)
{
    *col = C_LIGHT;
    switch ( r )
    {
    case SE_BRIGHT: (void)snprintf(v, cap, "%d%%", set_bright); break;
    case SE_REST:
        if ( REST_S[set_rest] ) { (void)snprintf(v, cap, "%d s", REST_S[set_rest]); }
        else { (void)snprintf(v, cap, "Never"); }
        break;
    case SE_AOD: (void)snprintf(v, cap, "%s", set_aod ? "On" : "Off"); *col = set_aod ? C_OK : C_MED; break;
    case SE_SOUND: (void)snprintf(v, cap, "%s", sound_on() ? "On" : "Off"); *col = sound_on() ? C_OK : C_MED; break;
    case SE_RAISE: (void)snprintf(v, cap, "%s", set_raise ? "On" : "Off"); *col = set_raise ? C_OK : C_MED; break;
    case SE_TAPW: (void)snprintf(v, cap, "%s", set_tapwake ? "On" : "Off"); *col = set_tapwake ? C_OK : C_MED; break;
    case SE_GOAL: (void)snprintf(v, cap, "%d", set_goal); break;
    case SE_FACE: (void)snprintf(v, cap, "%s", FACE_NAME[set_face % FACES]); break;
    default: (void)snprintf(v, cap, "%s >", KW_VERSION); *col = C_MED; break;
    }
}

static void draw_settings(void)
{
    static const char *const K[SE_N] = { "Brightness", "Screen off after", "Always-on time", "Sound", "Raise to wake",
                                         "Tap to wake", "Step goal", "Watch face", "About" };
    for ( int r = 0; r < SE_N; ++r )
    {
        char v[24];
        uint16_t col;
        se_value(r, v, sizeof(v), &col);
        int const y = SE_Y0 + r * SE_RH;
        kk_round(18, y, KK_W - 36, SE_RH - 6, 20, C_ROW);
        text(&KF_BODY, 40, y + (SE_RH - 6 - KF_BODY.cap) / 2, K[r], C_MED);
        text(&KF_NAME, KK_W - 40 - kf_width(&KF_NAME, v), y + (SE_RH - 6 - KF_NAME.cap) / 2, v, col);
    }
    back_bar("Settings");
}

static void tap_settings(int x, int y)
{
    (void)x;
    int const r = (y - SE_Y0) / SE_RH;
    if ( (y < SE_Y0) || (r < 0) || (r >= SE_N) ) { return; }
    static const int BR[4] = { 25, 50, 75, 100 };
    switch ( r )
    {
    case SE_BRIGHT:
    {
        int k = 0;
        while ( (k < 4) && (BR[k] <= set_bright) ) { ++k; }
        set_bright = BR[k % 4];
        apply_bright();
        break;
    }
    case SE_REST: set_rest = (set_rest + 1) % 5; break;
    case SE_AOD: set_aod = !set_aod; break;
    case SE_SOUND: sound_set_on(!sound_on()); sound_play(SND_NOTE); break;
    case SE_RAISE: set_raise = !set_raise; break;
    case SE_TAPW: set_tapwake = !set_tapwake; break;
    case SE_GOAL: set_goal = (set_goal >= 15000) ? 4000 : set_goal + 1000; break;
    case SE_FACE: set_face = (set_face + 1) % FACES; break;
    default: open_app(A_INFO); return;
    }
    sound_play(SND_CLICK);
    save_settings();
}

/* ---- Steps: today against the goal, and the week ---- */

static void draw_steps(void)
{
    hist_load();
    long const n = (st_day >= 0) ? today_steps() : 0;
    float const p = (set_goal > 0) ? (float)n / (float)set_goal : 0.0f;
    int const cy = 186;
    arc(CX, cy, 88, 102, 0.0f, PI2, C_OFF);
    if ( p > 0.0f ) { arc(CX, cy, 88, 102, 0.0f, PI2 * ((p > 1.0f) ? 1.0f : p), (p >= 1.0f) ? C_OK : C_YEL); }
    char t[16];
    (void)snprintf(t, sizeof(t), "%ld", n);
    text_c(&KF_TITLE, CX, cy - KF_TITLE.cap / 2 - 8, t, C_LIGHT);
    char g[24];
    (void)snprintf(g, sizeof(g), "of %d", set_goal);
    text_c(&KF_CAPTION, CX, cy + 22, g, C_MED);
    if ( !imu_ok() ) { text_c(&KF_CAPTION, CX, cy + 50, "no motion sensor", C_RED); }
    /* the week: six days back and today, the highest bar full */
    long v[ST_HIST];
    v[ST_HIST - 1] = n;
    for ( int i = 0; i < ST_HIST - 1; ++i ) { v[ST_HIST - 2 - i] = st_hist[i]; }
    long mx = set_goal;
    for ( int i = 0; i < ST_HIST; ++i ) { if ( v[i] > mx ) { mx = v[i]; } }
    kk_clock_t c;
    int const wd = kk_clock(&c) ? c.wday : -1;
    int const bw = 34, gap = 14, x0 = CX - (ST_HIST * bw + (ST_HIST - 1) * gap) / 2, base = 450, hmax = 120;
    int const gy = base - (int)((long)hmax * set_goal / mx);
    kk_rect(x0 - 6, gy, ST_HIST * bw + (ST_HIST - 1) * gap + 12, 2, C_OFF);   /* the goal line */
    for ( int i = 0; i < ST_HIST; ++i )
    {
        int const h = (int)((long)hmax * v[i] / mx);
        int const x = x0 + i * (bw + gap);
        uint16_t const col = (i == ST_HIST - 1) ? C_YEL : ((v[i] >= set_goal) ? C_OK : C_REST);
        if ( h > 0 ) { kk_round(x, base - h, bw, h, (h > 10) ? 8 : h / 2, col); }
        if ( wd >= 0 )
        {
            char d[2] = { DAY[((wd - (ST_HIST - 1 - i)) % 7 + 7) % 7][0], '\0' };
            text_c(&KF_CAPTION, x + bw / 2, base + 8, d, (i == ST_HIST - 1) ? C_LIGHT : C_MED);
        }
    }
    back_bar("Steps");
}

/* ---- the alarm clock ---- */

static void alarm_tick(uint32_t now)
{
    static uint32_t last_ring;
    if ( al_ring_until > now )
    {
        if ( (now - last_ring) > 1400U ) { last_ring = now; sound_play(SND_ALARM); wake(); }
        return;
    }
    al_ring_until = 0;
    kk_clock_t c;
    if ( !kk_clock(&c) || (0 != c.sec / 10) ) { return; }   /* the first 10 s of a minute */
    long const day = (long)c.year * 400L + (long)c.mon * 32L + (long)c.mday;
    bool const snooze = (al_snooze_h == c.hour) && (al_snooze_m == c.min);
    bool const set = al_on && (al_h == c.hour) && (al_m == c.min) && (!al_wd || ((c.wday >= 1) && (c.wday <= 5)));
    if ( snooze ) { al_snooze_h = -1; }
    else if ( !set || (al_done_day == day) ) { return; }
    else { al_done_day = day; }
    al_ring_until = now + 120000U;   /* two minutes, then it gives up */
    last_ring = 0;
    view = V_FACE;
    wake();
}

static void alarm_stop(bool snooze)
{
    al_ring_until = 0;
    if ( snooze )
    {
        kk_clock_t c;
        if ( kk_clock(&c) )
        {
            int const t = c.hour * 60 + c.min + 9;
            al_snooze_h = (t / 60) % 24;
            al_snooze_m = t % 60;
        }
    }
}

static bool draw_alarm_ring(uint32_t now)
{
    if ( al_ring_until <= now ) { return false; }
    bool const on = ((now / 500U) % 2U) == 0U;
    char t[8];
    kk_clock_t c;
    if ( kk_clock(&c) ) { (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min); }
    else { (void)snprintf(t, sizeof(t), "--:--"); }
    text_c(&KF_CAPTION, CX, 70, "Alarm", on ? C_YEL : C_MED);
    text_c(&KF_CLOCK, CX, 110, t, C_LIGHT);
    pill(26, 300, KK_W - 52, 80, "Snooze 9 min", C_OFF, C_LIGHT);
    pill(26, 396, KK_W - 52, 80, "Stop", C_YEL, C_SOFT);
    return true;
}

static bool tap_alarm_ring(int x, int y, uint32_t now)
{
    if ( al_ring_until <= now ) { return false; }
    if ( hit(x, y, 26, 300, KK_W - 52, 80) ) { alarm_stop(true); }
    else if ( hit(x, y, 26, 396, KK_W - 52, 80) ) { alarm_stop(false); }
    return true;
}

static void draw_alarm(void)
{
    char t[8];
    (void)snprintf(t, sizeof(t), "%02d:%02d", al_h, al_m);
    text_c(&KF_CLOCK, CX, 88, t, al_on ? C_LIGHT : C_REST);
    /* under the hours and under the minutes: - and + */
    pill(26, 200, 80, 60, "-", C_OFF, C_LIGHT);
    pill(114, 200, 80, 60, "+", C_OFF, C_LIGHT);
    pill(KK_W - 194, 200, 80, 60, "-", C_OFF, C_LIGHT);
    pill(KK_W - 106, 200, 80, 60, "+", C_OFF, C_LIGHT);
    text_c(&KF_CAPTION, 110, 266, "hour", C_REST);
    text_c(&KF_CAPTION, KK_W - 110, 266, "min", C_REST);
    pill(26, 300, KK_W - 52, 64, al_on ? "On" : "Off", al_on ? C_OK : C_OFF, al_on ? C_SOFT : C_LIGHT);
    pill(26, 376, KK_W - 52, 64, al_wd ? "Mon - Fri" : "Every day", C_ROW, C_LIGHT);
    if ( al_snooze_h >= 0 )
    {
        char sn[24];
        (void)snprintf(sn, sizeof(sn), "snoozed till %02d:%02d", al_snooze_h, al_snooze_m);
        text_c(&KF_CAPTION, CX, 452, sn, C_YEL);
    }
    back_bar("Alarm");
}

static void tap_alarm(int x, int y)
{
    bool ch = true;
    if ( hit(x, y, 114, 200, 80, 60) ) { al_h = (al_h + 1) % 24; }
    else if ( hit(x, y, 26, 200, 80, 60) ) { al_h = (al_h + 23) % 24; }
    else if ( hit(x, y, KK_W - 106, 200, 80, 60) ) { al_m = (al_m + 5) % 60; }
    else if ( hit(x, y, KK_W - 194, 200, 80, 60) ) { al_m = (al_m + 59) % 60; }
    else if ( hit(x, y, 26, 300, KK_W - 52, 64) ) { al_on = !al_on; }
    else if ( hit(x, y, 26, 376, KK_W - 52, 64) ) { al_wd = !al_wd; }
    else { ch = false; }
    if ( ch )
    {
        al_done_day = -1;   /* a new time may ring today */
        sound_play(SND_CLICK);
        save_settings();
    }
}

/* ---- Agenda: today and tomorrow from Home Assistant's calendars ---- */


static long cal_today(kk_clock_t const *c) { return (long)c->year * 10000L + (long)c->mon * 100L + (long)c->mday; }

/* day of the week, 0 = Sunday (Sakamoto) */
static int cal_wday(int y, int m, int d)
{
    static const int T[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if ( m < 3 ) { y -= 1; }
    return (y + y / 4 - y / 100 + y / 400 + T[(m - 1) % 12] + d) % 7;
}

static long cal_tomorrow(long ymd)
{
    static const int ML[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y = (int)(ymd / 10000), m = (int)(ymd / 100 % 100), d = (int)(ymd % 100);
    int ml = ML[(m - 1) % 12] + (((2 == m) && ((0 == y % 4) && ((0 != y % 100) || (0 == y % 400)))) ? 1 : 0);
    if ( ++d > ml ) { d = 1; if ( ++m > 12 ) { m = 1; ++y; } }
    return (long)y * 10000L + (long)m * 100L + d;
}

/* @p s cut with "..." so it fits in @p w pixels */
static void fit_text(const kf_font_t *f, char *s, int w)
{
    size_t n = strlen(s);
    if ( kf_width(f, s) <= w ) { return; }
    while ( (n > 1) )
    {
        --n;
        s[n] = '\0';
        char t[48];
        (void)snprintf(t, sizeof(t), "%s...", s);
        if ( kf_width(f, t) <= w ) { (void)snprintf(s, 48, "%s", t); return; }
    }
}

static void cal_when(cal_ev_t const *e, char *o, size_t cap)
{
    if ( e->s < 0 ) { (void)snprintf(o, cap, "all day"); }
    else { (void)snprintf(o, cap, "%02d:%02d", e->s / 60, e->s % 60); }
}

/* the next event still to come (today, then tomorrow); NULL when none */
static cal_ev_t const *cal_next(void)
{
    kk_clock_t c;
    if ( !kk_clock(&c) ) { return NULL; }
    long const today = cal_today(&c);
    int const now = c.hour * 60 + c.min;
    for ( int i = 0; i < cal_n; ++i )
    {
        cal_ev_t const *const e = &cal_ev[i];
        if ( (e->ymd == today) && (e->s >= 0) && (e->e > now) ) { return e; }
        if ( e->ymd > today ) { return e; }
    }
    return NULL;
}

static void draw_cal(void)
{
    kk_clock_t c;
    bool const ok = kk_clock(&c);
    long const today = ok ? cal_today(&c) : 0;
    int y = 78;
    long day = -1;
    int shown = 0;
    if ( !cal_ok ) { text_c(&KF_BODY, CX, 200, "no agenda yet", C_REST); }
    else if ( 0 == cal_n ) { text_c(&KF_BODY, CX, 200, "nothing planned", C_MED); }
    for ( int i = cal_off; (i < cal_n) && (y < 400); ++i )
    {
        cal_ev_t const *const e = &cal_ev[i];
        if ( e->ymd != day )
        {
            day = e->ymd;
            char hd[24];
            if ( day == today ) { (void)snprintf(hd, sizeof(hd), "TODAY"); }
            else if ( day == cal_tomorrow(today) ) { (void)snprintf(hd, sizeof(hd), "TOMORROW"); }
            else
            {
                int const yy = (int)(day / 10000), mm = (int)(day / 100 % 100), dd = (int)(day % 100);
                (void)snprintf(hd, sizeof(hd), "%s %d %s", DAY[cal_wday(yy, mm, dd)], dd, MON[(mm + 11) % 12]);
            }
            text(&KF_CAPTION, 40, y + 4, hd, C_REST);
            y += 30;
        }
        kk_round(24, y, KK_W - 48, 50, 16, C_ROW);
        kk_round(34, y + 12, 6, 26, 3, CAL_COL[e->c & 3]);
        char w[12];
        cal_when(e, w, sizeof(w));
        text(&KF_CAPTION, 52, y + (50 - KF_CAPTION.cap) / 2, w, C_MED);
        char t[48];
        (void)snprintf(t, sizeof(t), "%s", e->t);
        fit_text(&KF_BODY, t, KK_W - 48 - 140);
        text(&KF_BODY, 140, y + (50 - KF_BODY.cap) / 2, t, C_LIGHT);
        y += 56;
        ++shown;
    }
    if ( (cal_off + shown) < cal_n ) { text_c(&KF_CAPTION, CX, 410, "tap: more", C_REST); }
    back_bar("Agenda");
}

static void tap_cal(int x, int y)
{
    (void)x;
    if ( y < 76 ) { return; }
    cal_off = (cal_off + 5 < cal_n) ? cal_off + 5 : 0;
    sound_play(SND_CLICK);
}

/* ---- Music: Home Assistant's media players, through the hub ---- */

/* a filled triangle pointing right (dir 1) or left (-1), w wide, h high, middle at (cx, cy) */
static void tri(int cx, int cy, int w, int h, int dir, uint16_t c)
{
    for ( int i = 0; i < w; ++i )
    {
        int const hh = h * (w - i) / w;
        int const x = (dir > 0) ? (cx - w / 2 + i) : (cx + w / 2 - i);
        kk_rect(x, cy - hh / 2, 1, hh, c);
    }
}

#define MU_Y 250
#define MU_VY 360

static void draw_music(void)
{
    if ( !mu_ok ) { text_c(&KF_BODY, CX, 200, "no player yet", C_REST); back_bar("Music"); return; }
    /* the player: tap to switch */
    char p[48];
    (void)snprintf(p, sizeof(p), "%s  (%d/%d)", mu_player, mu_i + 1, mu_n);
    fit_text(&KF_CAPTION, p, KK_W - 80);
    kk_round(30, 74, KK_W - 60, 36, 16, C_ROW);
    text_c(&KF_CAPTION, CX, 92 - KF_CAPTION.cap / 2, p, C_MED);
    char t[64];
    (void)snprintf(t, sizeof(t), "%s", mu_title[0] ? mu_title : mu_state);
    fit_text(&KF_NAME, t, KK_W - 60);
    text_c(&KF_NAME, CX, 140, t, C_LIGHT);
    (void)snprintf(t, sizeof(t), "%s", mu_artist);
    fit_text(&KF_BODY, t, KK_W - 60);
    text_c(&KF_BODY, CX, 184, t, C_MED);
    /* prev, play/pause, next */
    bool const playing = (0 == strcmp(mu_state, "playing"));
    kk_disc(CX - 120, MU_Y, 36, C_ROW);
    tri(CX - 114, MU_Y, 18, 22, -1, C_LIGHT);
    kk_rect(CX - 133, MU_Y - 11, 4, 22, C_LIGHT);
    kk_disc(CX, MU_Y, 50, C_RED);
    if ( playing ) { kk_rect(CX - 14, MU_Y - 18, 10, 36, C_LIGHT); kk_rect(CX + 4, MU_Y - 18, 10, 36, C_LIGHT); }
    else { tri(CX + 4, MU_Y, 30, 36, 1, C_LIGHT); }
    kk_disc(CX + 120, MU_Y, 36, C_ROW);
    tri(CX + 114, MU_Y, 18, 22, 1, C_LIGHT);
    kk_rect(CX + 129, MU_Y - 11, 4, 22, C_LIGHT);
    /* volume */
    pill(26, MU_VY, 70, 56, "-", C_OFF, C_LIGHT);
    pill(KK_W - 96, MU_VY, 70, 56, "+", C_OFF, C_LIGHT);
    int const bw = KK_W - 2 * 112;
    kk_round(112, MU_VY + 24, bw, 8, 4, C_OFF);
    if ( mu_vol > 0 ) { kk_round(112, MU_VY + 24, bw * mu_vol / 100, 8, 4, C_LIGHT); }
    back_bar("Music");
}

static void tap_music(int x, int y)
{
    const char *cmd = NULL;
    if ( (y >= 70) && (y < 114) ) { cmd = "sel"; }
    else if ( (y >= MU_Y - 50) && (y < MU_Y + 50) )
    {
        if ( x < CX - 70 ) { cmd = "prev"; }
        else if ( x > CX + 70 ) { cmd = "next"; }
        else
        {
            cmd = "pp";
            (void)snprintf(mu_state, sizeof(mu_state), "%s", (0 == strcmp(mu_state, "playing")) ? "paused" : "playing");
        }
    }
    else if ( hit(x, y, 26, MU_VY, 70, 56) ) { cmd = "vol-"; if ( mu_vol >= 5 ) { mu_vol -= 5; } }
    else if ( hit(x, y, KK_W - 96, MU_VY, 70, 56) ) { cmd = "vol+"; if ( (mu_vol >= 0) && (mu_vol <= 95) ) { mu_vol += 5; } }
    if ( cmd )
    {
        kk_link_send("mu %s", cmd);
        sound_play(SND_CLICK);
    }
}

/* ---- Find iPhone: Home Assistant rings it (a critical alert, also in silent mode) ---- */

static uint32_t ph_ask_at;

static void draw_findph(void)
{
    uint32_t const now = kk_ms();
    char t[40];
    if ( ph_ok )
    {
        (void)snprintf(t, sizeof(t), "%s", ph_where);
        text_c(&KF_NAME, CX, 90, t, C_LIGHT);
        if ( ph_bat >= 0 ) { (void)snprintf(t, sizeof(t), "battery %d%%", ph_bat); text_c(&KF_CAPTION, CX, 130, t, C_MED); }
    }
    else { text_c(&KF_CAPTION, CX, 110, "asking the hub...", C_REST); }
    bool const rang = (ph_rang_at != 0) && ((now - ph_rang_at) < 6000U);
    bool const busy = !rang && (ph_ask_at != 0) && ((now - ph_ask_at) < 6000U);
    uint16_t const col = rang ? C_OK : (busy ? C_OFF : C_BLUE);
    kk_disc(CX, 270, 88, col);
    kk_ring(CX, 270, 104, 3, ((now / 400U) % 2U && busy) ? C_BLUE : C_ROW);
    kf_icon(g_fb, &KI_PIN_40, CX, 254, C_LIGHT);
    text_c(&KF_BODY, CX, 290, rang ? "ringing!" : (busy ? "calling..." : "Ring"), C_LIGHT);
    text_c(&KF_CAPTION, CX, 400, "it rings loud, also on silent", C_REST);
    back_bar("Find iPhone");
}

static void tap_findph(int x, int y)
{
    int const dx = x - CX, dy = y - 270;
    if ( (dx * dx + dy * dy) > (100 * 100) ) { return; }
    ph_ask_at = kk_ms();
    ph_rang_at = 0;
    kk_link_send("ph ring");
    sound_play(SND_CLICK);
}

/* ---- Sleep: movement through the night, from the motion sensor ---- */

#define SL_DEEP 10      /* below: lying still */
#define SL_LIGHT 30     /* below: asleep, turning; above: awake */

static void sl_load(void)
{
    static bool done;
    if ( done ) { return; }
    done = true;
    size_t n = sizeof(sl_b);
    sl_night = kk_best_get("sl_night");
    if ( !kk_blob_get("sl_b", sl_b, &n) || (n != sizeof(sl_b)) ) { memset(sl_b, 0, sizeof(sl_b)); sl_night = -1; }
}

static void sl_save(void)
{
    kk_best_put("sl_night", sl_night);
    kk_blob_put("sl_b", sl_b, sizeof(sl_b));
}

/* every minute: the 5-minute bucket of the night (20:00 .. 12:00) gets the movement */
static void sl_tick(uint32_t now)
{
    static uint32_t last;
    static int acc_sum, acc_n;
    if ( (now - last) < 60000U ) { return; }
    last = now;
    sl_load();
    int const a = imu_motion_take();
    kk_clock_t c;
    if ( (a < 0) || !kk_clock(&c) ) { return; }
    int const mod = c.hour * 60 + c.min;
    int const rel = (mod + 24 * 60 - 20 * 60) % (24 * 60);   /* minutes since 20:00 */
    if ( rel >= SL_N * 5 ) { acc_sum = acc_n = 0; return; }  /* 12:00 .. 20:00: day */
    long ev = cal_today(&c);
    if ( c.hour < 20 )   /* after midnight: the night belongs to yesterday's evening */
    {
        int y = c.year, mo = c.mon, d = c.mday - 1;
        if ( 0 == d ) { --mo; if ( 0 == mo ) { mo = 12; --y; } static const int ML[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; d = ML[mo - 1] + ((2 == mo) && (0 == y % 4) ? 1 : 0); }
        ev = (long)y * 10000L + (long)mo * 100L + d;
    }
    if ( ev != sl_night ) { memset(sl_b, 0, sizeof(sl_b)); sl_night = ev; sl_last = -1; }
    int const i = rel / 5;
    if ( i != sl_last ) { acc_sum = acc_n = 0; sl_last = i; }
    acc_sum += a;
    ++acc_n;
    int v = docked ? 255 : (acc_sum / acc_n);
    if ( v < 1 ) { v = 1; }
    if ( v > 254 && !docked ) { v = 254; }
    sl_b[i] = (uint8_t)v;
    if ( (now - sl_dirty_at) > 30U * 60000U ) { sl_dirty_at = now; sl_save(); }
    if ( 0 == c.min % 30 ) { ESP_LOGI("os", "sleep: bucket %d move %d", i, v); }
}

/* the night's sleep: first and last bucket of a quiet stretch (>= 20 min), and the asleep minutes between */
static bool sl_summary(int *from, int *to, int *mins, int *deep)
{
    int f = -1, t = -1;
    for ( int i = 0; i + 3 < SL_N; ++i )
    {
        bool q = true;
        for ( int k = 0; k < 4; ++k ) { int const v = sl_b[i + k]; if ( (0 == v) || (v >= SL_LIGHT) ) { q = false; break; } }
        if ( q ) { if ( f < 0 ) { f = i; } t = i + 3; }
    }
    if ( f < 0 ) { return false; }
    int m = 0, d = 0;
    for ( int i = f; i <= t; ++i ) { int const v = sl_b[i]; if ( (v > 0) && (v < SL_LIGHT) ) { m += 5; if ( v < SL_DEEP ) { d += 5; } } }
    *from = f; *to = t; *mins = m; *deep = d;
    return true;
}

static void draw_sleep(void)
{
    sl_load();
    int f, t, m, d;
    char s1[32];
    if ( sl_summary(&f, &t, &m, &d) )
    {
        (void)snprintf(s1, sizeof(s1), "%dh %02dm", m / 60, m % 60);
        text_c(&KF_TITLE, CX, 84, s1, C_LIGHT);
        int const a = 20 * 60 + f * 5, b = 20 * 60 + (t + 1) * 5;
        (void)snprintf(s1, sizeof(s1), "%02d:%02d - %02d:%02d   still %dh %02dm", (a / 60) % 24, a % 60, (b / 60) % 24, b % 60, d / 60, d % 60);
        text_c(&KF_CAPTION, CX, 150, s1, C_MED);
    }
    else { text_c(&KF_BODY, CX, 110, "no night yet", C_REST); }
    /* the night: one bar per 5 minutes, 20:00 .. 12:00 */
    int const x0 = 22, w = KK_W - 44, base = 330, hmax = 120;
    for ( int i = 0; i < SL_N; ++i )
    {
        int const v = sl_b[i];
        int const x = x0 + i * w / SL_N, x2 = x0 + (i + 1) * w / SL_N;
        if ( 0 == v ) { continue; }
        int h;
        uint16_t col;
        if ( 255 == v ) { h = 8; col = C_CHG; }
        else if ( v < SL_DEEP ) { h = hmax; col = C_BLUE; }
        else if ( v < SL_LIGHT ) { h = hmax * 6 / 10; col = KK_RGB(0x60, 0x9C, 0xD8); }
        else { h = hmax / 4; col = C_YEL; }
        kk_rect(x, base - h, (x2 - x > 1) ? x2 - x : 1, h, col);
    }
    kk_rect(x0, base + 2, w, 2, C_OFF);
    static const char *const LB[5] = { "20", "00", "04", "08", "12" };
    for ( int k = 0; k < 5; ++k ) { text_c(&KF_CAPTION, x0 + k * w / 4, base + 10, LB[k], C_REST); }
    kk_round(40, 392, 16, 16, 4, C_BLUE);  text(&KF_CAPTION, 62, 392, "still", C_MED);
    kk_round(150, 392, 16, 16, 4, KK_RGB(0x60, 0x9C, 0xD8)); text(&KF_CAPTION, 172, 392, "light", C_MED);
    kk_round(262, 392, 16, 16, 4, C_YEL); text(&KF_CAPTION, 284, 392, "awake", C_MED);
    back_bar("Sleep");
}

/* ---- Battery: the last 24 hours ---- */

static long bt_now_slot(kk_clock_t const *c)
{
    /* days since 2000 (good enough for gaps), then 10-minute steps */
    static const int CUM[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long const y = c->year - 2000;
    long const days = y * 365L + (y + 3) / 4 + CUM[(c->mon + 11) % 12] + ((c->mon > 2) && (0 == c->year % 4) ? 1 : 0) + c->mday - 1;
    return days * 144L + (c->hour * 60 + c->min) / 10;
}

static void bt_load(void)
{
    static bool done;
    if ( done ) { return; }
    done = true;
    size_t n = sizeof(bt_h);
    bt_slot = kk_best_get("bt_slot");
    if ( !kk_blob_get("bt_h", bt_h, &n) || (n != sizeof(bt_h)) ) { memset(bt_h, 0, sizeof(bt_h)); bt_slot = -1; }
}

static void bt_tick(uint32_t now)
{
    static uint32_t last, saved_at;
    if ( ((now - last) < 30000U) || (battery < 0) ) { return; }
    last = now;
    bt_load();
    kk_clock_t c;
    if ( !kk_clock(&c) ) { return; }
    long const sl = bt_now_slot(&c);
    if ( (bt_slot < 0) || (sl - bt_slot >= BT_N) || (sl < bt_slot) ) { memset(bt_h, 0, sizeof(bt_h)); }
    else { for ( long k = bt_slot + 1; k < sl; ++k ) { bt_h[k % BT_N] = 0; } }   /* off: no data */
    bool const fresh = (sl != bt_slot);
    bt_slot = sl;
    int const p = (battery > 100) ? 100 : (battery < 1 ? 1 : battery);
    bt_h[sl % BT_N] = (uint8_t)(p | (charging ? 0x80 : 0));
    if ( fresh && ((now - saved_at) > 60U * 60000U) )
    {
        saved_at = now;
        kk_best_put("bt_slot", bt_slot);
        kk_blob_put("bt_h", bt_h, sizeof(bt_h));
    }
}

static void draw_batt(void)
{
    bt_load();
    char t[40];
    (void)snprintf(t, sizeof(t), "%d%%", (battery < 0) ? 0 : battery);
    text_c(&KF_TITLE, CX, 80, t, charging ? C_CHG : ((battery < 20) ? C_RED : C_LIGHT));
    /* time left: the drop over the last 2 hours without the charger */
    int k = 0, first = -1, last = -1;
    for ( int i = 0; (i < 12) && (bt_slot >= 0); ++i )
    {
        uint8_t const v = bt_h[(bt_slot - i + BT_N) % BT_N];
        if ( (0 == v) || (v & 0x80) ) { break; }
        if ( last < 0 ) { last = v; }
        first = v;
        k = i;
    }
    if ( charging ) { (void)snprintf(t, sizeof(t), "charging"); }
    else if ( (k >= 3) && (first > last) )
    {
        int const hrs = last * k * 10 / (first - last) / 60;
        (void)snprintf(t, sizeof(t), "about %d h left", hrs);
    }
    else { (void)snprintf(t, sizeof(t), "measuring..."); }
    text_c(&KF_CAPTION, CX, 140, t, C_MED);
    /* 24 h: oldest left, now right */
    int const x0 = 30, w = KK_W - 60, base = 340, hmax = 150;
    kk_rect(x0, base - hmax, w, 1, C_ROW);
    kk_rect(x0, base - hmax / 2, w, 1, C_ROW);
    for ( int i = 0; (i < BT_N) && (bt_slot >= 0); ++i )
    {
        uint8_t const v = bt_h[(bt_slot - (BT_N - 1) + i + BT_N * 2) % BT_N];
        if ( 0 == v ) { continue; }
        int const p = v & 0x7F;
        int const x = x0 + i * w / BT_N, x2 = x0 + (i + 1) * w / BT_N;
        int const h = hmax * p / 100;
        kk_rect(x, base - h, (x2 - x > 1) ? x2 - x : 1, h, (v & 0x80) ? C_CHG : ((p < 20) ? C_RED : C_OK));
    }
    kk_rect(x0, base + 1, w, 2, C_OFF);
    text(&KF_CAPTION, x0, base + 10, "24 h ago", C_REST);
    text(&KF_CAPTION, x0 + w - kf_width(&KF_CAPTION, "now"), base + 10, "now", C_REST);
    text(&KF_CAPTION, x0 - 4, base - hmax - 22, "100%", C_REST);
    back_bar("Battery");
}

static void draw_app(void)
{
    switch ( app )
    {
    case A_NOTIF: draw_notif(); break;
    case A_TIMER: draw_timer(); break;
    case A_HOME: draw_home(); break;
#if CONFIG_BT_ENABLED
    case A_PHONE: draw_phone(); break;
#endif
    case A_RC: draw_rc(kk_ms()); break;
    case A_STORE: draw_store(kk_ms()); break;
    case A_FILES: draw_files(kk_ms()); break;
    case A_FOLDER: draw_folder(); break;
    case A_SET: draw_settings(); break;
    case A_STEPS: draw_steps(); break;
    case A_ALARM: draw_alarm(); break;
    case A_CAL: draw_cal(); break;
    case A_MUSIC: draw_music(); break;
    case A_FINDPH: draw_findph(); break;
    case A_SLEEP: draw_sleep(); break;
    case A_BATT: draw_batt(); break;
    case A_CRAFT: draw_craft(); break;
    case A_WIFI:
        wifi_ui_draw();
        if ( !wifi_ui_modal() ) { back_bar("Wi-Fi"); }
        break;
    default: draw_info(); break;
    }
}

static void tap_app(int x, int y, uint32_t now)
{
    switch ( app )
    {
    case A_NOTIF: tap_notif(x, y); break;
    case A_TIMER: tap_timer(x, y); break;
    case A_HOME: tap_home(x, y); break;
#if CONFIG_BT_ENABLED
    case A_PHONE: tap_phone(x, y, now); break;
#endif
    case A_RC: tap_rc(x, y); break;
    case A_STORE: tap_store(x, y, now); break;
    case A_FILES: tap_files(x, y, now); break;
    case A_FOLDER: tap_folder(x, y); break;
    case A_SET: tap_settings(x, y); break;
    case A_ALARM: tap_alarm(x, y); break;
    case A_CAL: tap_cal(x, y); break;
    case A_MUSIC: tap_music(x, y); break;
    case A_FINDPH: tap_findph(x, y); break;
    case A_CRAFT: tap_craft(x, y); break;
    case A_WIFI: wifi_ui_tap(x, y); break;
    default: break;
    }
}

static void draw_banner(void)
{
    kk_round(18, 24, KK_W - 36, 96, 32, C_ROW);
    kk_disc(18 + 48, 72, 30, C_BLUE);
    kf_icon(g_fb, &KI_LIST_40, 18 + 48, 72, C_SOFT);
    text(&KF_CAPTION, 18 + 94, 42, note_app[nnotes - 1][0] ? note_app[nnotes - 1] : "Notification", C_MED);
    char line[NLEN];
    fit(&KF_NAME, notes[nnotes - 1], KK_W - 36 - 94 - 24, line, sizeof(line));
    text(&KF_NAME, 18 + 94, 72, line, C_LIGHT);
}

/* ---- input ---- */

enum { G_NONE, G_TAP, G_LEFT, G_RIGHT, G_UP, G_DOWN };

static void back(void)
{
    if ( V_APP == view )
    {
        if ( (A_WIFI == app) && wifi_ui_modal() ) { wifi_ui_back(); }
        else if ( (A_NOTIF == app) && (note_open >= 0) ) { note_open = -1; }
        else if ( (A_RC == app) || (A_CRAFT == app) ) { open_app(A_FOLDER); }
        else if ( A_INFO == app ) { open_app(A_SET); }
        else { view = V_APPS; }
    }
    else { view = V_FACE; }
}

static void on_gesture(int g, int x, int y, uint32_t now)
{
    bool const typing = (V_APP == view) && (A_WIFI == app) && wifi_ui_modal();
    if ( typing )
    {
        if ( G_TAP == g ) { wifi_ui_tap(x, y); }
        return;
    }
    switch ( view )
    {
    case V_FACE:
        if ( G_UP == g ) { rail_u = rail_target = 0.0f; view = V_APPS; }
        else if ( G_DOWN == g ) { view = V_CC; }
        else if ( (G_LEFT == g) || (G_RIGHT == g) )
        {
            set_face = (set_face + ((G_LEFT == g) ? 1 : 2)) % 3;
            save_settings();
            sound_play(SND_CLICK);
        }
        else if ( G_TAP == g )
        {
            if ( (x > 320) && (y > 76) && (y < 168) ) { open_app(A_STEPS); }
            else if ( y >= KODY_Y ) { kody_play(KODY_LOOKS); }
            else if ( (y >= 160) && (y < 210) && nnotes ) { open_app(A_NOTIF); }
        }
        break;
    case V_APPS:
        if ( G_RIGHT == g ) { view = V_FACE; }
        else if ( G_TAP == g )
        {
            int const i = rail_at(y);
            if ( i >= 0 ) { launch(i); }
        }
        break;
    case V_CC:
        if ( (G_UP == g) || (G_LEFT == g) || (G_RIGHT == g) ) { view = V_FACE; }
        else if ( G_TAP == g ) { tap_cc(x, y, now); }
        break;
    default:
        if ( (A_FOLDER == app) && ((G_UP == g) || (G_DOWN == g) || (G_LEFT == g)) )
        {
            fo_off += ((G_UP == g) || (G_LEFT == g)) ? 1 : -1;
        }
        else if ( (A_FILES == app) && ((G_UP == g) || (G_DOWN == g)) )
        {
            fl_off += (G_UP == g) ? 3 : -3;
            if ( fl_off < 0 ) { fl_off = 0; }
        }
        else if ( (A_STORE == app) && ((G_UP == g) || (G_DOWN == g)) )
        {
            st_off += (G_UP == g) ? 1 : -1;      /* swipe up: further down the list */
            if ( st_off < 0 ) { st_off = 0; }
            st_rm = -1;
        }
        else if ( G_RIGHT == g ) { back(); }
        else if ( G_TAP == g )
        {
            if ( (y < BAR_FOOT) && !((A_WIFI == app) && wifi_ui_modal()) ) { back(); }
            else { tap_app(x, y, now); }
        }
        break;
    }
}

void app_main(void)
{
    kk_init();
    sound_init();
    imu_init();
    (void)kody_init(NULL);
    storage_init();
    load_settings();
    rlog_init();
    crash_init();
    fs_init();
    ble_init();
    (void)esp_task_wdt_add(NULL);
    {
        esp_pm_config_t const pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
        if ( (ESP_OK == esp_pm_configure(&pm)) && (ESP_OK == esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "os", &s_fast)) )
        {
            (void)esp_pm_lock_acquire(s_fast);
        }
        else { s_fast = NULL; }
    }   /* the OS loop: a hang reboots (and reports) after 20 s */
    last_input = kk_ms();
    apply_bright();
    bool touching = false, swallow = false, dragging = false;
    int sx = 0, sy = 0, lx = 0, ly = 0;
    float u0 = 0.0f;
    for ( ;; )
    {
        g_fb = kk_begin();
        (void)esp_task_wdt_reset();
        uint32_t const now = kk_ms();
        {
            static uint32_t fps_at, fps_n;
            ++fps_n;
            if ( (now - fps_at) >= 10000U )
            {
                ESP_LOGI("os", "fps %lu, internal free %u", (unsigned long)(fps_n * 1000U / (now - fps_at)),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                fps_at = now;
                fps_n = 0;
            }
        }
        char m[512];
        while ( kk_link_recv(m, sizeof(m)) ) { on_link(m); }
        while ( ble_recv(m, sizeof(m)) ) { on_ble(m); }
        tick(now);
        alarm_tick(now);
        sl_tick(now);
        bt_tick(now);
        fs_tick();
        rc_tick(now);
        if ( app_get_id[0] && !apps_loading() )
        {
            apps_item_t li[APPS_MAX];
            int const n = apps_list(li, APPS_MAX);
            int k = -1;
            for ( int i = 0; i < n; ++i ) { if ( 0 == strcmp(li[i].id, app_get_id) ) { k = i; } }
            kk_link_send("app get %s %d", app_get_id, (k >= 0) && apps_install(k, STORE_BASE));
            app_get_id[0] = '\0';
        }
        {
            static uint32_t last_ring;
            if ( (alarm_until > now) && ((now - last_ring) > 1400U) ) { last_ring = now; sound_play(SND_ALARM); wake(); }
            /* raise the wrist: the screen comes back */
            if ( set_raise && imu_raised() && resting ) { wake(); }
        }
        {
            /* Tell the hub what runs here, each time it comes up; and once the
             * OS has run 30 s, keep this image (an update is rolled back otherwise). */
            static bool hub_was, good;
            bool const up = net_hub_up();
            if ( up && !hub_was )
            {
                kk_link_send("ver %s %s", KW_VERSION, ota_slot());
                if ( kk_link_send("%s", crash_report()) ) { crash_sent(); }
                storage_info_t si;
                storage_info(&si);
                kk_link_send("sd %s %s", si.sd_ok ? "ok" : "fail", storage_sd_status());
            }
            hub_was = up;
            /* a fresh update is kept only once it reached the hub; no hub in 3 minutes: restart, the old one comes back */
            if ( !good && (now > 30000U) && up ) { good = true; ota_mark_good(); }
            else if ( !good && (now > 180000U) && ota_is_pending() ) { ESP_LOGW("os", "update never reached the hub: rolling back"); esp_restart(); }
        }
        send_status(now);

        /* touch */
        int tx = 0, ty = 0;
        bool const down = (kk_touch(&tx, &ty) || remote_touch(now, &tx, &ty)) && !launching;
        if ( down && !touching && (alarm_until > now) )
        {
            alarm_until = 0;          /* a touch stops the alarm, and is nothing else */
            touching = true;
            swallow = true;
            sx = lx = tx;
            sy = ly = ty;
            wake();
        }
        else if ( down && !touching )
        {
            touching = true;
            sx = lx = tx;
            sy = ly = ty;
            u0 = rail_u;
            dragging = false;
            swallow = resting || (find_until > now);
            bool const keep_dark = resting && !set_tapwake;
            if ( find_until > now ) { find_until = 0; }
            if ( !resting && (tap_alarm_ring(tx, ty, now) || tap_modal(tx, ty)) ) { swallow = true; }
            drag_level = 0;
            if ( !swallow && (V_APP == view) && (A_RC == app)
                 && (((tx - CX) * (tx - CX) + (ty - RC_CY) * (ty - RC_CY)) <= (RC_R + 20) * (RC_R + 20)) )
            {
                rc_stick = true;
                rc_stick_at(tx, ty);
            }
            if ( !swallow && (V_CC == view) )
            {
                if ( hit(tx, ty, LV_X, LV1_Y, LV_W, LV_H) ) { drag_level = 1; }
                else if ( hit(tx, ty, LV_X, LV2_Y, LV_W, LV_H) ) { drag_level = 2; }
                if ( drag_level ) { cc_level_drag(tx); }
            }
            if ( !keep_dark ) { wake(); }
        }
        else if ( down )
        {
            lx = tx;
            ly = ty;
            last_input = now;
            if ( !swallow && rc_stick ) { rc_stick_at(lx, ly); }
            if ( !swallow && drag_level ) { cc_level_drag(lx); }
            else if ( !swallow && (V_APPS == view) )
            {
                int const dy = ly - sy;
                if ( dragging || (dy > 12) || (dy < -12) )
                {
                    dragging = true;
                    float u = u0 - (float)dy;
                    float const mx = (float)rail_max();
                    if ( u < 0.0f ) { u *= 0.35f; }                          /* rubber at the ends */
                    if ( u > mx ) { u = mx + (u - mx) * 0.35f; }
                    rail_u = rail_target = u;
                }
            }
        }
        else if ( touching )
        {
            touching = false;
            int const dx = lx - sx, dy = ly - sy;
            int const ax = (dx < 0) ? -dx : dx, ay = (dy < 0) ? -dy : dy;
            int g = G_NONE;
            if ( (dx * dx + dy * dy) < 900 ) { g = G_TAP; }
            else if ( (ax > 70) && (ax > ay) ) { g = (dx > 0) ? G_RIGHT : G_LEFT; }
            else if ( (ay > 70) && (ay > ax) ) { g = (dy > 0) ? G_DOWN : G_UP; }
            if ( dragging && ((G_UP == g) || (G_DOWN == g)) ) { g = G_NONE; }
            if ( rc_stick )
            {
                /* the stick lets go: stop now, and no swipe out of the app */
                rc_stick = false;
                rc_thr = rc_str = 0;
                kk_link_send("drive 0 0");
                g = G_NONE;
            }
            if ( drag_level )
            {
                save_settings();
                drag_level = 0;
                g = G_NONE;
            }
            if ( V_APPS == view )
            {
                /* settle on the nearest row */
                float t = roundf(rail_u / ROW_H) * ROW_H;
                if ( t < 0.0f ) { t = 0.0f; }
                if ( t > (float)rail_max() ) { t = (float)rail_max(); }
                rail_target = t;
            }
            if ( !swallow && (G_NONE != g) ) { on_gesture(g, lx, ly, now); }
        }
        if ( (V_APPS == view) && !touching ) { rail_u += (rail_target - rail_u) * 0.35f; }

        /* buttons: TOP is home (or the apps from the face), BOT is back */
        static uint32_t top_down_at;
        static bool top_held, top_long;
        if ( kk_pressed(KK_TOP) ) { top_down_at = now; top_held = true; top_long = false; }
        if ( top_held && !top_long && ((now - top_down_at) >= 600U) && !launching )
        {
            top_long = true;              /* held: the control centre, at once */
            find_until = 0;
            wake();
            view = V_CC;
        }
        bool p_top = rem_top;
        if ( top_held && kk_released(KK_TOP) )
        {
            top_held = false;
            if ( !top_long ) { p_top = true; }
        }
        bool const p_bot = kk_pressed(KK_BOT) || rem_bot;
        rem_top = rem_bot = false;
        if ( (p_top || p_bot) && (al_ring_until > now) )
        {
            alarm_stop(p_top);
        }
        else if ( (p_top || p_bot) && !launching )
        {
            bool const was = resting || (find_until > now);
            bool const top = p_top;
            find_until = 0;
            wake();
            if ( !was )
            {
                if ( top )
                {
                    if ( V_FACE == view ) { rail_u = rail_target = 0.0f; view = V_APPS; }
                    else { view = V_FACE; }
                }
                else { back(); }
            }
        }

        maybe_rest(now);

        /* draw */
        if ( resting )
        {
            kk_clock_t c;
            int const mn = kk_clock(&c) ? c.min : -1;
            if ( !set_aod )
            {
                if ( !rest_dark )
                {
                    rest_dark = true;
                    kk_clear(KK_BLACK);
                    board_panel(false);
                    kk_end();
                }
                else { vTaskDelay(pdMS_TO_TICKS(100)); }
                continue;
            }
            if ( (mn == rest_drawn_min) && (nnotes == rest_drawn_notes) && (battery == rest_drawn_bat) )
            {
                kk_skip();      /* nothing changed: keep the last frame */
                vTaskDelay(pdMS_TO_TICKS(60));
                continue;
            }
            rest_drawn_min = mn;
            rest_drawn_notes = nnotes;
            rest_drawn_bat = battery;
            kk_clear(KK_BLACK);
            draw_rest();
            kk_end();
            continue;
        }

        kk_clear(KK_BLACK);
        if ( ota_progress() >= 0 )
        {
            char op[16];
            (void)snprintf(op, sizeof(op), "%d%%", ota_progress());
            text_c(&KF_TITLE, CX, 170, "Updating", C_LIGHT);
            text_c(&KF_FIGURE, CX, 230, op, C_OK);
            text_c(&KF_CAPTION, CX, 300, "keep the watch on Wi-Fi", C_MED);
            kk_end();
            continue;
        }
        if ( find_until > now )
        {
            bool const on = ((now / 250U) % 2U) == 0U;
            kk_clear(on ? C_BLUE : KK_BLACK);
            text_c(&KF_TITLE, CX, 200, "Here I am!", on ? C_SOFT : C_LIGHT);
            text_c(&KF_CAPTION, CX, 260, "tap to stop", on ? C_SOFT : C_MED);
            kk_end();
            continue;
        }
        if ( draw_alarm_ring(now) || draw_modal(now) )
        {
            kk_end();
            continue;
        }
        if ( launching )
        {
            if ( draw_launch(now) )
            {
                launching = false;
                open_app(launch_app);
                kk_clear(KK_BLACK);
                draw_app();
            }
            kk_end();
            continue;
        }
        switch ( view )
        {
        case V_FACE: draw_face(now); break;
        case V_APPS: draw_apps(0); break;
        case V_CC: draw_cc(now); break;
        default: draw_app(); break;
        }
        if ( (banner_until > now) && (nnotes > 0) ) { draw_banner(); }
        kk_end();
    }
}

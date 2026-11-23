/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Kode Watch: the watch's home app. A face, notifications, a timer and
 *        home controls; swipe left/right (or BOT) to change screen, TOP acts.
 *        A paired Kode Dot drives it over the Kode Link (see the docs page).
 *
 * Link messages in:  notify <text> | find | go <0..3> | next | prev |
 *                    timer add <s> | timer toggle | timer reset |
 *                    home <0..3> | sys dock <0|1> | hub peer <0|1>
 * Link messages out: st <hh:mm> <bat> <dock> <screen> <steps> <notifs> <timer-s> <timer-run> <home-bits>
 *                    home <name> <0|1>   (for Home Assistant, via the hub)
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("Kode Watch", "tools");

#define CX (KK_W / 2)
#define NSCREEN 4
#define NMAX 8
#define NLEN 64

enum { FACE, NOTIF, TIMER, HOME };
static const char *const SCREEN_NAME[NSCREEN] = { "WIJZER", "MELDINGEN", "TIMER", "HUIS" };
static const char *const ROOM[4] = { "WOONKAMER", "KEUKEN", "SLAAPKAMER", "ALLES UIT" };
static const char *const DAY[7] = { "ZO", "MA", "DI", "WO", "DO", "VR", "ZA" };
static const char *const MON[12] = { "JAN", "FEB", "MRT", "APR", "MEI", "JUN", "JUL", "AUG", "SEP", "OKT", "NOV", "DEC" };

static int screen;
static bool docked, linked;
static float battery = 72.0f;
#ifdef KW_HW
static long steps = 0;        /* QMI8658 step counter: later */
static bool last_power;
#else
static long steps = 6240;
#endif
static char notes[NMAX][NLEN];
static uint32_t note_at[NMAX];
static int nnotes;
static uint32_t banner_until;
static uint32_t find_until;
static int timer_set = 300, timer_left = 300;
static bool timer_run;
static uint32_t timer_tick, alarm_until;
static bool home_on[3];
static int home_sel;
static uint32_t last_status, last_sim;

/* ---- the model ---- */

static void add_note(const char *t)
{
    if ( nnotes == NMAX )
    {
        memmove(notes[0], notes[1], sizeof(notes[0]) * (NMAX - 1));
        memmove(&note_at[0], &note_at[1], sizeof(note_at[0]) * (NMAX - 1));
        --nnotes;
    }
    int i = 0;
    for ( ; t[i] && (i < NLEN - 1); ++i ) { notes[nnotes][i] = (char)toupper((unsigned char)t[i]); }
    notes[nnotes][i] = '\0';
    note_at[nnotes] = kk_ms();
    ++nnotes;
    banner_until = kk_ms() + 3500;
    kk_buzz(250);
    kk_tone(1320, 60);
    kk_led(0, 40, 80);
}

static void timer_toggle(void)
{
    if ( timer_left <= 0 ) { timer_left = timer_set; }
    timer_run = !timer_run;
    timer_tick = kk_ms();
    kk_tone(timer_run ? 880 : 440, 40);
}

static void timer_add(int s)
{
    timer_left += s;
    if ( timer_left > 99 * 60 ) { timer_left = 99 * 60; }
    if ( !timer_run ) { timer_set = timer_left; }
}

static void home_toggle(int i)
{
    if ( i == 3 )
    {
        for ( int k = 0; k < 3; ++k ) { home_on[k] = false; }
        kk_link_send("home alles 0");
    }
    else if ( (i >= 0) && (i < 3) )
    {
        home_on[i] = !home_on[i];
        kk_link_send("home %s %d", ROOM[i], home_on[i] ? 1 : 0);
    }
    kk_tone(660, 25);
}

static void go(int s)
{
    screen = ((s % NSCREEN) + NSCREEN) % NSCREEN;
    kk_tone(520, 15);
}

static void on_link(const char *m)
{
    int a = 0;
    if ( 0 == strncmp(m, "notify ", 7) ) { add_note(m + 7); }
    else if ( 0 == strcmp(m, "find") ) { find_until = kk_ms() + 4000; }
    else if ( 1 == sscanf(m, "go %d", &a) ) { go(a); }
    else if ( 0 == strcmp(m, "next") ) { go(screen + 1); }
    else if ( 0 == strcmp(m, "prev") ) { go(screen - 1); }
    else if ( 1 == sscanf(m, "timer add %d", &a) ) { timer_add(a); }
    else if ( 0 == strcmp(m, "timer toggle") ) { timer_toggle(); }
    else if ( 0 == strcmp(m, "timer reset") ) { timer_run = false; timer_left = timer_set = 300; }
    else if ( 1 == sscanf(m, "home %d", &a) ) { home_toggle(a); }
    else if ( 1 == sscanf(m, "sys dock %d", &a) ) { docked = (a != 0); if ( docked ) { screen = FACE; } kk_tone(docked ? 990 : 660, 40); }
    else if ( 1 == sscanf(m, "hub peer %d", &a) ) { linked = (a != 0); }
    else if ( 0 == strcmp(m, "clear") ) { nnotes = 0; }
}

static void simulate(uint32_t now)
{
    if ( (now - last_sim) < 1000U ) { return; }
    last_sim = now;
#ifdef KW_HW
    /* The real thing: the AXP2101 gauge, and USB power means the dock. */
    int const b = kk_battery();
    if ( b >= 0 ) { battery = (float)b; }
    bool const p = kk_power_in();
    if ( p != last_power )
    {
        last_power = p;
        docked = p;
        if ( docked ) { screen = FACE; }
        kk_bright(docked ? 25 : 100);
    }
#else
    /* A quick battery so the preview shows both ways: drain on the wrist, charge in the dock. */
    battery += docked ? 1.5f : -0.08f;
    if ( battery > 100.0f ) { battery = 100.0f; }
    if ( battery < 1.0f ) { battery = 1.0f; }
    if ( !docked && (kk_rand(3) == 0) ) { steps += 1 + (long)kk_rand(9); }
#endif
    if ( timer_run )
    {
        while ( (now - timer_tick) >= 1000U )
        {
            timer_tick += 1000U;
            if ( --timer_left <= 0 )
            {
                timer_left = 0;
                timer_run = false;
                alarm_until = now + 4000;
                kk_buzz(600);
                add_note("TIMER AFGELOPEN");
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
    kk_link_send("st %s %d %d %d %ld %d %d %d %d", hm, (int)battery, docked ? 1 : 0, screen, steps, nnotes, timer_left,
                 timer_run ? 1 : 0, (home_on[0] ? 1 : 0) | (home_on[1] ? 2 : 0) | (home_on[2] ? 4 : 0));
}

/* ---- drawing ---- */

static void status_bar(void)
{
    char b[16];
    (void)snprintf(b, sizeof(b), "%d%%", (int)battery);
    uint16_t const bc = (battery < 20) ? KK_RED : (docked ? KK_YELLOW : KK_GREEN);
    kk_text(CX + 18, 40, b, 2, bc);
    kk_box(CX - 34, 40, 44, 16, 2, KK_GREY);
    kk_rect(CX + 10, 45, 3, 6, KK_GREY);
    kk_rect(CX - 31, 43, (int)(38 * battery / 100.0f), 10, bc);
    kk_text(CX - 112, 40, "BT", 2, linked ? KK_BLUE : KK_RGB(50, 54, 62));
    if ( nnotes ) { kk_disc(CX + 104, 47, 7, KK_CYAN); }
}

static void dots(void)
{
    for ( int i = 0; i < NSCREEN; ++i )
    {
        kk_disc(CX - 30 + i * 20, 472, (i == screen) ? 5 : 3, (i == screen) ? KK_WHITE : KK_GREY);
    }
}

static void draw_face(void)
{
    kk_clock_t c;
    bool const ok = kk_clock(&c);
    char t[24];
    if ( docked )
    {
        /* Nightstand: big, dim, warm. */
        uint16_t const col = KK_RGB(200, 90, 40);
        (void)snprintf(t, sizeof(t), ok ? "%02d:%02d" : "--:--", ok ? c.hour : 0, ok ? c.min : 0);
        kk_text_c(190, t, 13, col);
        (void)snprintf(t, sizeof(t), "LADEN %d%%", (int)battery);
        kk_text_c(310, t, 2, KK_RGB(120, 60, 30));
        return;
    }
    status_bar();
    if ( ok )
    {
        (void)snprintf(t, sizeof(t), "%s %d %s", DAY[c.wday], c.mday, MON[c.mon - 1]);
        kk_text_c(120, t, 3, KK_GREY);
        (void)snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min);
        kk_text_c(170, t, 12, KK_WHITE);
        /* seconds: a bar under the time */
        kk_rect(CX - 150, 270, 300, 4, KK_RGB(30, 34, 42));
        kk_rect(CX - 150, 270, 300 * c.sec / 59, 4, KK_ORANGE);
    }
    else
    {
        kk_text_c(190, "--:--", 12, KK_GREY);
        kk_text_c(280, "WACHT OP TIJD", 2, KK_GREY);
    }
    (void)snprintf(t, sizeof(t), "%ld STAPPEN", steps);
    kk_text_c(300, t, 3, KK_ORANGE);
    /* three complications */
    struct { const char *v; const char *l; uint16_t c; } cp[3];
    char nb[8], tb[8];
    (void)snprintf(nb, sizeof(nb), "%d", nnotes);
    (void)snprintf(tb, sizeof(tb), "%d:%02d", timer_left / 60, timer_left % 60);
    cp[0].v = nb; cp[0].l = "MELD"; cp[0].c = KK_CYAN;
    cp[1].v = tb; cp[1].l = "TIMER"; cp[1].c = timer_run ? KK_GREEN : KK_GREY;
    cp[2].v = home_on[0] || home_on[1] || home_on[2] ? "AAN" : "UIT"; cp[2].l = "HUIS"; cp[2].c = KK_YELLOW;
    for ( int i = 0; i < 3; ++i )
    {
        int const x = CX - 110 + i * 110;
        kk_disc(x, 380, 42, KK_RGB(20, 23, 30));
        kk_text(x - kk_text_w(cp[i].v, 2) / 2, 366, cp[i].v, 2, cp[i].c);
        kk_text(x - kk_text_w(cp[i].l, 1) / 2, 392, cp[i].l, 1, KK_GREY);
    }
}

static void draw_notif(void)
{
    status_bar();
    kk_text_c(80, "MELDINGEN", 3, KK_CYAN);
    if ( 0 == nnotes )
    {
        kk_text_c(230, "GEEN MELDINGEN", 2, KK_GREY);
        return;
    }
    int y = 125;
    for ( int i = nnotes - 1; (i >= 0) && (y < 420); --i, y += 58 )
    {
        kk_round(48, y, KK_W - 96, 50, 12, KK_RGB(22, 26, 34));
        char line[24];
        (void)snprintf(line, sizeof(line), "%.20s", notes[i]);
        kk_text(62, y + 10, line, 2, KK_WHITE);
        uint32_t const ago = (kk_ms() - note_at[i]) / 60000U;
        (void)snprintf(line, sizeof(line), ago ? "%lu MIN GELEDEN" : "NU", (unsigned long)ago);
        kk_text(62, y + 32, line, 1, KK_GREY);
    }
    kk_text_c(430, "TOP: WIS", 2, KK_GREY);
}

static void draw_timer(void)
{
    status_bar();
    kk_text_c(80, "TIMER", 3, KK_GREEN);
    char t[16];
    (void)snprintf(t, sizeof(t), "%02d:%02d", timer_left / 60, timer_left % 60);
    kk_ring(CX, 240, 120, 10, KK_RGB(30, 34, 42));
    if ( timer_set > 0 )
    {
        int const n = 60 * timer_left / timer_set;
        for ( int i = 0; i < n; ++i )
        {
            float const a = (float)i * 6.2831853f / 60.0f;
            int const x = CX + (int)(sinf(a) * 120), y = 240 - (int)(cosf(a) * 120);
            kk_disc(x, y, 5, timer_run ? KK_GREEN : KK_GREY);
        }
    }
    kk_text_c(220, t, 7, (alarm_until > kk_ms()) ? KK_RED : KK_WHITE);
    kk_text_c(390, timer_run ? "TOP: PAUZE" : "TOP: START", 2, KK_WHITE);
    kk_text_c(418, "TIK: +1 MIN", 2, KK_GREY);
}

static void draw_home(void)
{
    status_bar();
    kk_text_c(80, "HUIS", 3, KK_YELLOW);
    for ( int i = 0; i < 4; ++i )
    {
        int const x = 52 + (i % 2) * 158, y = 125 + (i / 2) * 150;
        bool const on = (i < 3) && home_on[i];
        uint16_t const bg = on ? KK_RGB(90, 70, 10) : KK_RGB(22, 26, 34);
        kk_round(x, y, 148, 138, 18, bg);
        if ( i == home_sel ) { kk_box(x, y, 148, 138, 3, KK_WHITE); }
        kk_disc(x + 74, y + 50, 22, on ? KK_YELLOW : KK_RGB(60, 64, 72));
        char nm[12];
        (void)snprintf(nm, sizeof(nm), "%.10s", ROOM[i]);
        kk_text(x + 74 - kk_text_w(nm, 2) / 2, y + 96, nm, 2, on ? KK_WHITE : KK_GREY);
    }
    kk_text_c(430, "TIK OF TOP: SCHAKEL", 1, KK_GREY);
}

static int home_at(int x, int y)
{
    for ( int i = 0; i < 4; ++i )
    {
        int const bx = 52 + (i % 2) * 158, by = 125 + (i / 2) * 150;
        if ( (x >= bx) && (x < bx + 148) && (y >= by) && (y < by + 138) ) { return i; }
    }
    return -1;
}

void app_main(void)
{
    kk_init();
    bool swiping = false;
    int sx = 0, sy = 0, lx = 0, ly = 0;
    for ( ;; )
    {
        kk_begin();
        uint32_t const now = kk_ms();
        char m[512];
        while ( kk_link_recv(m, sizeof(m)) ) { on_link(m); }

        /* Swipes and taps. */
        int tx, ty;
        bool const down = kk_touch(&tx, &ty);
        if ( down && !swiping ) { swiping = true; sx = lx = tx; sy = ly = ty; }
        else if ( down ) { lx = tx; ly = ty; }
        else if ( swiping )
        {
            swiping = false;
            int const dx = lx - sx, dy = ly - sy;
            if ( (dx > 70) && (dx > (dy < 0 ? -dy : dy)) ) { go(screen - 1); }
            else if ( (dx < -70) && (-dx > (dy < 0 ? -dy : dy)) ) { go(screen + 1); }
            else if ( (dx * dx + dy * dy) < 400 )
            {
                if ( docked ) { /* in the dock a tap only wakes the screen */ }
                else if ( TIMER == screen ) { timer_add(60); kk_tone(700, 20); }
                else if ( HOME == screen ) { int const h = home_at(lx, ly); if ( h >= 0 ) { home_sel = h; home_toggle(h); } }
                else if ( NOTIF == screen ) { nnotes = 0; }
            }
        }
        if ( kk_pressed(KK_BOT) ) { go(screen + 1); }
        if ( kk_pressed(KK_TOP) )
        {
            if ( find_until > now ) { find_until = 0; }
            else if ( TIMER == screen ) { timer_toggle(); }
            else if ( NOTIF == screen ) { if ( nnotes ) { --nnotes; } }
            else if ( HOME == screen ) { home_toggle(home_sel); home_sel = (home_sel + 1) % 4; }
            else { go(NOTIF); }
        }
        simulate(now);
        send_status(now);
        if ( (banner_until > now) || (alarm_until > now) || (find_until > now) ) { /* keep the LED */ }
        else { kk_led(0, 0, 0); }

        /* Draw. */
        kk_clear(KK_BLACK);
        if ( find_until > now )
        {
            bool const on = ((now / 250U) % 2U) == 0U;
            kk_clear(on ? KK_BLUE : KK_BLACK);
            kk_text_c(200, "HIER!", 8, KK_WHITE);
            kk_text_c(300, "TOP: STOP", 2, KK_WHITE);
            if ( on ) { kk_tone(1760, 120); kk_led(0, 60, 120); } else { kk_led(0, 0, 0); }
            kk_end();
            continue;
        }
        switch ( screen )
        {
        case FACE: draw_face(); break;
        case NOTIF: draw_notif(); break;
        case TIMER: draw_timer(); break;
        default: draw_home(); break;
        }
        if ( !docked ) { dots(); }
        if ( (banner_until > now) && (nnotes > 0) )
        {
            kk_round(40, 60, KK_W - 80, 76, 16, KK_RGB(8, 60, 90));
            kk_text(58, 72, "MELDING", 1, KK_CYAN);
            char line[24];
            (void)snprintf(line, sizeof(line), "%.20s", notes[nnotes - 1]);
            kk_text(58, 92, line, 2, KK_WHITE);
        }
        (void)SCREEN_NAME;
        kk_end();
    }
}

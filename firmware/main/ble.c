/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ble.c
 * @brief The iPhone over Bluetooth LE (see ble.h).
 *
 * The watch is the peripheral; once the link is encrypted it is also the GATT
 * client of the phone's Apple services:
 *   ANCS  notifications: Notification Source tells of one, the Control Point
 *         asks its app, title and message, the Data Source answers.
 *   AMS   music: Entity Update tells the track and the player's state, the
 *         Remote Command plays, pauses and skips.
 *   CTS   the phone's local time.
 *
 * Every GATT call is made from the NimBLE host task: other tasks queue an
 * operation and post an event; one operation is on the air at a time.
 */
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_BT_ENABLED
/* Bluetooth is off in this build: the OS sees a radio that never comes up. */
#include "ble.h"
#include <string.h>
void ble_init(void) {}
bool ble_recv(char *buf, int cap) { (void)buf; (void)cap; return false; }
void ble_status(ble_status_t *st) { memset(st, 0, sizeof(*st)); st->playing = -1; }
void ble_pair_answer(bool yes) { (void)yes; }
void ble_music(int cmd) { (void)cmd; }
void ble_notif_action(uint32_t uid, bool positive) { (void)uid; (void)positive; }
void ble_forget(void) {}
void ble_stop(void) {}
#else
#include "ble.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_central.h"
#include "esp_coexist.h"

#define TAG "ble"

void ble_store_config_init(void);

/* ---- UUIDs ---- */

static ble_uuid_any_t U_ANCS, U_NS, U_CP, U_DS, U_AMS, U_RC, U_EU;
static const ble_uuid16_t U_CTS = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t U_CT = BLE_UUID16_INIT(0x2A2B);
static const ble_uuid16_t U_GAP = BLE_UUID16_INIT(0x1800);
static const ble_uuid16_t U_NAME = BLE_UUID16_INIT(0x2A00);
static const ble_uuid16_t U_CCCD = BLE_UUID16_INIT(0x2902);
static ble_uuid128_t s_sol;   /* ANCS, solicited in the advert */

/* ---- state ---- */

static SemaphoreHandle_t s_mu;
static ble_status_t s_st;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_pk_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own;
static uint16_t h_ns, h_cp, h_ds, h_rc, h_eu, h_ct, h_name;

#define NCALL 4
static uint32_t s_calls[NCALL];

/* lines for the OS */
#define RQ 12
#define RLEN 200
EXT_RAM_BSS_ATTR static char s_rq[RQ][RLEN];
static unsigned s_rh, s_rt;

/* the GATT operations, one at a time */
enum { OP_WRITE, OP_READ };
typedef struct {
    uint8_t kind;
    uint8_t len;
    uint16_t h;
    uint8_t d[20];
} op_t;
#define NOPS 24
static op_t s_ops[NOPS];
static unsigned s_oh, s_ot;
static bool s_busy;
static struct ble_npl_event s_kick;

/* the Data Source's answer, in pieces */
EXT_RAM_BSS_ATTR static uint8_t s_ds[640];
static int s_dsn;

static void adv(void);
static int gap_event(struct ble_gap_event *ev, void *arg);

#define LOCK() xSemaphoreTake(s_mu, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mu)

static void push_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void push_line(const char *fmt, ...)
{
    LOCK();
    if ( (s_rh - s_rt) < RQ )
    {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(s_rq[s_rh % RQ], RLEN, fmt, ap);
        va_end(ap);
        ++s_rh;
    }
    UNLOCK();
}

bool ble_recv(char *buf, int cap)
{
    if ( NULL == s_mu ) { return false; }
    bool got = false;
    LOCK();
    if ( s_rh != s_rt )
    {
        (void)snprintf(buf, (size_t)cap, "%s", s_rq[s_rt % RQ]);
        ++s_rt;
        got = true;
    }
    UNLOCK();
    return got;
}

void ble_status(ble_status_t *st)
{
    if ( NULL == s_mu ) { memset(st, 0, sizeof(*st)); st->playing = -1; return; }
    LOCK();
    *st = s_st;
    UNLOCK();
}

/* ---- text: UTF-8 folded to the watch's ASCII ---- */

static void fold(const uint8_t *s, int n, char *out, int cap)
{
    static const char L1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int o = 0;
    for ( int i = 0; (i < n) && (o < cap - 4); )
    {
        uint32_t c = s[i];
        int k = 1;
        if ( c >= 0xF0 ) { k = 4; c &= 0x07; }
        else if ( c >= 0xE0 ) { k = 3; c &= 0x0F; }
        else if ( c >= 0xC0 ) { k = 2; c &= 0x1F; }
        for ( int j = 1; (j < k) && (i + j < n); ++j ) { c = (c << 6) | (s[i + j] & 0x3F); }
        i += k;
        if ( 0 == c ) { break; }
        if ( (c == '\n') || (c == '\r') || (c == '\t') || (c == '|') ) { c = ' '; }
        if ( c < 0x20 ) { continue; }
        if ( c < 0x7F ) { out[o++] = (char)c; }
        else if ( (c >= 0xC0) && (c <= 0xFF) ) { out[o++] = L1[c - 0xC0]; }
        else if ( (0x2018 == c) || (0x2019 == c) ) { out[o++] = '\''; }
        else if ( (0x201C == c) || (0x201D == c) ) { out[o++] = '"'; }
        else if ( (0x2013 == c) || (0x2014 == c) ) { out[o++] = '-'; }
        else if ( 0x2026 == c ) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
        else if ( (0xA0 == c) || (0x202F == c) ) { out[o++] = ' '; }
        /* the rest (emoji and other scripts) has no glyph: left out */
    }
    while ( (o > 0) && (' ' == out[o - 1]) ) { --o; }
    out[o] = '\0';
}

static const char *app_name(const char *id)
{
    static const struct { const char *id, *name; } M[] = {
        { "com.apple.MobileSMS", "Messages" }, { "com.apple.mobilephone", "Phone" },
        { "com.apple.mobilemail", "Mail" }, { "com.apple.mobilecal", "Calendar" },
        { "com.apple.reminders", "Reminders" }, { "com.apple.facetime", "FaceTime" },
        { "net.whatsapp.WhatsApp", "WhatsApp" }, { "ph.telegra.Telegraph", "Telegram" },
        { "com.burbn.instagram", "Instagram" }, { "com.toyopagroup.picaboo", "Snapchat" },
        { "com.hammerandchisel.discord", "Discord" }, { "com.google.Gmail", "Gmail" },
        { "com.spotify.client", "Spotify" }, { "com.zhiliaoapp.musically", "TikTok" },
    };
    for ( size_t i = 0; i < sizeof(M) / sizeof(M[0]); ++i ) { if ( 0 == strcmp(id, M[i].id) ) { return M[i].name; } }
    const char *d = strrchr(id, '.');
    return d ? d + 1 : id;
}

/* ---- the operation queue ---- */

static void pump(void);

static void op_push(uint8_t kind, uint16_t h, const uint8_t *d, int len)
{
    if ( (0 == h) || (len > (int)sizeof(s_ops[0].d)) ) { return; }
    LOCK();
    if ( (s_oh - s_ot) < NOPS )
    {
        op_t *o = &s_ops[s_oh % NOPS];
        o->kind = kind;
        o->h = h;
        o->len = (uint8_t)len;
        if ( len ) { memcpy(o->d, d, (size_t)len); }
        ++s_oh;
    }
    UNLOCK();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_kick);
}

static void subscribe(const struct peer *p, const ble_uuid_t *svc, const ble_uuid_t *chr)
{
    const struct peer_dsc *d = peer_dsc_find_uuid(p, svc, chr, &U_CCCD.u);
    static const uint8_t on[2] = { 0x01, 0x00 };
    if ( d ) { op_push(OP_WRITE, d->dsc.handle, on, 2); }
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t const era = (y >= 0 ? y : y - 399) / 400;
    int64_t const yoe = y - era * 400;
    int64_t const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void on_time(const uint8_t *b, int n)
{
    if ( n < 7 ) { return; }
    int const y = b[0] | (b[1] << 8);
    if ( (y < 2020) || (b[2] < 1) || (b[2] > 12) || (b[3] < 1) ) { return; }
    int64_t const t = days_from_civil(y, b[2], b[3]) * 86400 + b[4] * 3600 + b[5] * 60 + b[6];
    push_line("btime %lld", (long long)t);
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)arg;
    if ( (0 == err->status) && attr && attr->om )
    {
        uint8_t b[64];
        uint16_t n = 0;
        (void)ble_hs_mbuf_to_flat(attr->om, b, sizeof(b), &n);
        if ( attr->handle == h_name )
        {
            char nm[32];
            fold(b, n, nm, sizeof(nm));
            LOCK();
            (void)snprintf(s_st.peer, sizeof(s_st.peer), "%s", nm);
            UNLOCK();
        }
        else if ( attr->handle == h_ct ) { on_time(b, n); }
    }
    if ( BLE_HS_EDONE == err->status ) { return 0; }
    s_busy = false;
    pump();
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)arg;
    if ( err->status ) { ESP_LOGW(TAG, "write 0x%04x: %d", attr ? attr->handle : 0, err->status); }
    s_busy = false;
    pump();
    return 0;
}

static void pump(void)
{
    while ( !s_busy && (BLE_HS_CONN_HANDLE_NONE != s_conn) )
    {
        op_t o;
        LOCK();
        bool const have = (s_oh != s_ot);
        if ( have ) { o = s_ops[s_ot % NOPS]; ++s_ot; }
        UNLOCK();
        if ( !have ) { return; }
        int rc = (OP_READ == o.kind) ? ble_gattc_read(s_conn, o.h, on_read, NULL)
                                     : ble_gattc_write_flat(s_conn, o.h, o.d, o.len, on_write, NULL);
        if ( 0 == rc ) { s_busy = true; }
        else { ESP_LOGW(TAG, "op 0x%04x: %d", o.h, rc); }
    }
}

static void kick(struct ble_npl_event *ev)
{
    (void)ev;
    pump();
}

/* ---- the Apple services ---- */

static bool is_call(uint32_t uid)
{
    for ( int i = 0; i < NCALL; ++i ) { if ( s_calls[i] == uid ) { return true; } }
    return false;
}

static void ancs_source(const uint8_t *d, int n)
{
    if ( n < 8 ) { return; }
    uint8_t const ev = d[0], flags = d[1], cat = d[2];
    uint32_t const uid = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    if ( 0 == ev )
    {
        if ( flags & 0x04 ) { return; }   /* pre-existing: already seen on the phone */
        if ( 1 == cat )
        {
            for ( int i = 0; i < NCALL; ++i ) { if ( 0 == s_calls[i] ) { s_calls[i] = uid; break; } }
        }
        /* app id, title (40), message (90) */
        uint8_t q[12] = { 0, d[4], d[5], d[6], d[7], 0, 1, 40, 0, 3, 90, 0 };
        op_push(OP_WRITE, h_cp, q, sizeof(q));
    }
    else if ( 2 == ev )
    {
        for ( int i = 0; i < NCALL; ++i ) { if ( s_calls[i] == uid ) { s_calls[i] = 0; } }
        push_line("bnrm %lu", (unsigned long)uid);
    }
}

static void ancs_data(const uint8_t *d, int n)
{
    if ( s_dsn + n > (int)sizeof(s_ds) ) { s_dsn = 0; }
    if ( n > (int)sizeof(s_ds) ) { return; }
    memcpy(s_ds + s_dsn, d, (size_t)n);
    s_dsn += n;
    for ( ;; )
    {
        if ( s_dsn < 5 ) { return; }
        if ( 0 != s_ds[0] ) { s_dsn = 0; return; }
        uint32_t const uid = s_ds[1] | (s_ds[2] << 8) | (s_ds[3] << 16) | ((uint32_t)s_ds[4] << 24);
        char app[40] = "", title[48] = "", msg[100] = "";
        int p = 5;
        for ( int got = 0; got < 3; ++got )
        {
            if ( p + 3 > s_dsn ) { return; }   /* the rest is on its way */
            int const id = s_ds[p], len = s_ds[p + 1] | (s_ds[p + 2] << 8);
            if ( p + 3 + len > s_dsn ) { return; }
            char *dst = (0 == id) ? app : (1 == id) ? title : msg;
            int const cap = (0 == id) ? sizeof(app) : (1 == id) ? sizeof(title) : sizeof(msg);
            if ( 0 == id )
            {
                int const k = (len < cap - 1) ? len : cap - 1;
                memcpy(app, s_ds + p + 3, (size_t)k);
                app[k] = '\0';
            }
            else { fold(s_ds + p + 3, len, dst, cap); }
            p += 3 + len;
        }
        if ( is_call(uid) ) { push_line("bcall %lu %s", (unsigned long)uid, title[0] ? title : "Unknown"); }
        else { push_line("bnote %lu %s|%s|%s", (unsigned long)uid, app_name(app), title, msg); }
        memmove(s_ds, s_ds + p, (size_t)(s_dsn - p));
        s_dsn -= p;
    }
}

static void ams_update(const uint8_t *d, int n)
{
    if ( n < 3 ) { return; }
    char v[48];
    fold(d + 3, n - 3, v, sizeof(v));
    LOCK();
    if ( (0 == d[0]) && (1 == d[1]) ) { s_st.playing = (n > 3) ? ((d[3] == '1') ? 1 : 0) : -1; }
    else if ( (2 == d[0]) && (0 == d[1]) ) { (void)snprintf(s_st.artist, sizeof(s_st.artist), "%s", v); }
    else if ( (2 == d[0]) && (2 == d[1]) ) { (void)snprintf(s_st.title, sizeof(s_st.title), "%s", v); }
    UNLOCK();
}

static void on_disc(const struct peer *p, int status, void *arg)
{
    (void)arg;
    if ( 0 != status ) { ESP_LOGW(TAG, "discovery: %d", status); }
    const struct peer_chr *c;
    h_name = (c = peer_chr_find_uuid(p, &U_GAP.u, &U_NAME.u)) ? c->chr.val_handle : 0;
    h_ct = (c = peer_chr_find_uuid(p, &U_CTS.u, &U_CT.u)) ? c->chr.val_handle : 0;
    h_ns = (c = peer_chr_find_uuid(p, &U_ANCS.u, &U_NS.u)) ? c->chr.val_handle : 0;
    h_cp = (c = peer_chr_find_uuid(p, &U_ANCS.u, &U_CP.u)) ? c->chr.val_handle : 0;
    h_ds = (c = peer_chr_find_uuid(p, &U_ANCS.u, &U_DS.u)) ? c->chr.val_handle : 0;
    h_rc = (c = peer_chr_find_uuid(p, &U_AMS.u, &U_RC.u)) ? c->chr.val_handle : 0;
    h_eu = (c = peer_chr_find_uuid(p, &U_AMS.u, &U_EU.u)) ? c->chr.val_handle : 0;
    ESP_LOGI(TAG, "found: ancs %d ams %d cts %d", h_ns && h_cp && h_ds, h_rc && h_eu, h_ct != 0);
    LOCK();
    s_st.ancs = h_ns && h_cp && h_ds;
    s_st.ams = h_rc && h_eu;
    s_st.cts = (0 != h_ct);
    UNLOCK();

    if ( h_name ) { op_push(OP_READ, h_name, NULL, 0); }
    if ( h_ct ) { op_push(OP_READ, h_ct, NULL, 0); subscribe(p, &U_CTS.u, &U_CT.u); }
    if ( h_eu )
    {
        subscribe(p, &U_AMS.u, &U_EU.u);
        static const uint8_t track[3] = { 2, 0, 2 }, player[2] = { 0, 1 };
        op_push(OP_WRITE, h_eu, track, 3);
        op_push(OP_WRITE, h_eu, player, 2);
    }
    /* the Data Source before the Notification Source, so no answer is missed */
    if ( h_ds ) { subscribe(p, &U_ANCS.u, &U_DS.u); }
    if ( h_ns ) { subscribe(p, &U_ANCS.u, &U_NS.u); }

    /* Discovery done: a slow link from now on, so Wi-Fi keeps the radio.
     * Within Apple's rules: max >= min + 15 ms, max * (latency + 1) * 3 < timeout. */
    struct ble_gap_upd_params const up = { .itvl_min = 96, .itvl_max = 144, .latency = 2,
                                           .supervision_timeout = 500, .min_ce_len = 0, .max_ce_len = 0 };
    int const rc = ble_gap_update_params(p->conn_handle, &up);
    if ( rc ) { ESP_LOGW(TAG, "conn params %d", rc); }
}

/* ---- GAP ---- */

static void forget_conn(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_busy = false;
    h_ns = h_cp = h_ds = h_rc = h_eu = h_ct = h_name = 0;
    s_dsn = 0;
    memset(s_calls, 0, sizeof(s_calls));
    LOCK();
    s_oh = s_ot = 0;
    s_st.connected = s_st.secure = false;
    s_st.ancs = s_st.ams = s_st.cts = false;
    s_st.numcmp_pending = s_st.passkey_show = false;
    s_st.playing = -1;
    s_st.title[0] = s_st.artist[0] = '\0';
    UNLOCK();
}

static void count_bonds(void)
{
    ble_addr_t a[2];
    int n = 0;
    (void)ble_store_util_bonded_peers(a, &n, 2);
    LOCK();
    s_st.bonded_any = (n > 0);
    UNLOCK();
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch ( ev->type )
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect %d", ev->connect.status);
        if ( 0 != ev->connect.status ) { adv(); return 0; }
        s_conn = ev->connect.conn_handle;
        (void)peer_add(s_conn);
        LOCK();
        s_st.connected = true;
        UNLOCK();
        (void)ble_gap_security_initiate(s_conn);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect 0x%x", ev->disconnect.reason);
        (void)peer_delete(ev->disconnect.conn.conn_handle);
        forget_conn();
        count_bonds();
        adv();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %d", ev->enc_change.status);
        if ( 0 != ev->enc_change.status )
        {
            /* the phone forgot us (or we it): drop our half and start over */
            if ( 0 == ble_gap_conn_find(ev->enc_change.conn_handle, &desc) ) { (void)ble_store_util_delete_peer(&desc.peer_id_addr); }
            (void)ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        LOCK();
        s_st.secure = true;
        s_st.numcmp_pending = s_st.passkey_show = false;
        UNLOCK();
        count_bonds();
        if ( 0 != peer_disc_all(ev->enc_change.conn_handle, on_disc, NULL) ) { ESP_LOGW(TAG, "discovery did not start"); }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if ( 0 == ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) ) { (void)ble_store_util_delete_peer(&desc.peer_id_addr); }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        s_pk_conn = ev->passkey.conn_handle;
        if ( BLE_SM_IOACT_NUMCMP == ev->passkey.params.action )
        {
            LOCK();
            s_st.numcmp = ev->passkey.params.numcmp;
            s_st.numcmp_pending = true;
            s_st.passkey_show = false;
            UNLOCK();
            push_line("bpair");
        }
        else if ( BLE_SM_IOACT_DISP == ev->passkey.params.action )
        {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000U };
            LOCK();
            s_st.numcmp = io.passkey;
            s_st.passkey_show = true;
            s_st.numcmp_pending = false;
            UNLOCK();
            (void)ble_sm_inject_io(s_pk_conn, &io);
            push_line("bpair");
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
    {
        uint8_t b[256];
        uint16_t n = 0;
        (void)ble_hs_mbuf_to_flat(ev->notify_rx.om, b, sizeof(b), &n);
        uint16_t const h = ev->notify_rx.attr_handle;
        if ( h && (h == h_ns) ) { ancs_source(b, n); }
        else if ( h && (h == h_ds) ) { ancs_data(b, n); }
        else if ( h && (h == h_eu) ) { ams_update(b, n); }
        else if ( h && (h == h_ct) ) { on_time(b, n); }
        return 0;
    }

    default:
        return 0;
    }
}

static void adv(void)
{
    if ( BLE_HS_CONN_HANDLE_NONE != s_conn ) { return; }
    if ( ble_gap_adv_active() ) { return; }
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = 0x00C1;   /* sports watch */
    f.appearance_is_present = 1;
    f.sol_uuids128 = &s_sol;
    f.sol_num_uuids128 = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if ( rc ) { ESP_LOGW(TAG, "adv fields %d", rc); }
    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    const char *nm = ble_svc_gap_device_name();
    r.name = (const uint8_t *)nm;
    r.name_len = (uint8_t)strlen(nm);
    r.name_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* paired: the iPhone finds a slow advert; unpaired: quicker to show up in Settings */
    bool bonded;
    LOCK();
    bonded = s_st.bonded_any;
    UNLOCK();
    p.itvl_min = bonded ? 1600 : 400;   /* 1 s : 250 ms */
    p.itvl_max = bonded ? 1760 : 480;
    rc = ble_gap_adv_start(s_own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if ( rc ) { ESP_LOGW(TAG, "adv start %d", rc); }
}

static void on_sync(void)
{
    (void)ble_hs_util_ensure_addr(0);
    (void)ble_hs_id_infer_auto(0, &s_own);
    count_bonds();
    LOCK();
    s_st.ready = true;
    UNLOCK();
    adv();
    ESP_LOGI(TAG, "up, internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "reset %d", reason);
    LOCK();
    s_st.ready = false;
    UNLOCK();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- the OS's calls ---- */

void ble_pair_answer(bool yes)
{
    uint16_t const c = s_pk_conn;
    bool was;
    LOCK();
    was = s_st.numcmp_pending;
    s_st.numcmp_pending = false;
    s_st.passkey_show = false;
    UNLOCK();
    if ( !was || (BLE_HS_CONN_HANDLE_NONE == c) ) { return; }
    struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = yes ? 1 : 0 };
    (void)ble_sm_inject_io(c, &io);
}

void ble_music(int cmd)
{
    uint8_t const b = (uint8_t)cmd;
    op_push(OP_WRITE, h_rc, &b, 1);
}

void ble_notif_action(uint32_t uid, bool positive)
{
    uint8_t const q[6] = { 2, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24), positive ? 0 : 1 };
    op_push(OP_WRITE, h_cp, q, sizeof(q));
}

void ble_forget(void)
{
    if ( BLE_HS_CONN_HANDLE_NONE != s_conn ) { (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM); }
    (void)ble_store_clear();
    count_bonds();
}

static bool s_running;

void ble_stop(void)
{
    if ( !s_running ) { return; }
    s_running = false;
    if ( 0 == nimble_port_stop() ) { (void)nimble_port_deinit(); }
    ESP_LOGI(TAG, "stopped, internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void ble_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_st.playing = -1;
    (void)ble_uuid_from_str(&U_ANCS, "7905f431-b5ce-4e99-a40f-4b1e122d00d0");
    (void)ble_uuid_from_str(&U_NS, "9fbf120d-6301-42d9-8c58-25e699a21dbd");
    (void)ble_uuid_from_str(&U_CP, "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9");
    (void)ble_uuid_from_str(&U_DS, "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb");
    (void)ble_uuid_from_str(&U_AMS, "89d3502b-0f36-433a-8ef4-c502ad55f8dc");
    (void)ble_uuid_from_str(&U_RC, "9b3c81d8-57b1-4a8a-b8df-0e56f7ca51c2");
    (void)ble_uuid_from_str(&U_EU, "2f7cabce-808d-411f-9a0c-bb92ba96c102");
    s_sol = U_ANCS.u128;

    esp_err_t const e = nimble_port_init();
    (void)esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    if ( ESP_OK != e ) { ESP_LOGE(TAG, "nimble init %d", e); return; }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set("Kode Watch");
    (void)ble_svc_gap_device_appearance_set(0x00C1);
    if ( 0 != peer_init(1, 48, 160, 200) ) { ESP_LOGE(TAG, "peer pools"); }
    ble_store_config_init();
    ble_npl_event_init(&s_kick, kick, NULL);
    nimble_port_freertos_init(host_task);
    s_running = true;
}

#endif /* CONFIG_BT_ENABLED */

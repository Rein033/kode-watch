/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file apps.c
 * @brief See apps.h. The directory and the request are written as kdrv_apps.c
 *        writes them: two directory copies, the other one each time, the magic
 *        last; the request in its own sector with a CRC of magic..size.
 */
#include "apps.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "psa/crypto.h"

#include "nvs.h"

#include "kapps_fmt.h"
#include "kk.h"

static const char *TAG = "apps";
#define SECTOR 0x1000U
#define META "/data/apps.txt"

static const esp_partition_t *s_part;
EXT_RAM_BSS_ATTR static kapps_dir_t s_dir;
static uint32_t s_dir_at = KAPPS_DIR_A_OFF;
static bool s_dir_ok;
static SemaphoreHandle_t s_lock;

EXT_RAM_BSS_ATTR static apps_item_t s_cat[APPS_MAX];
static int s_ncat;
static volatile bool s_loading;
static volatile int s_pct = -1;
static char s_err[48];
static char s_url[200];
static int s_want = -1;

/* ---- the directory ---- */

static uint32_t dir_crc(const kapps_dir_t *d) { return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)d, offsetof(kapps_dir_t, crc)); }

static bool dir_load(void)
{
    if ( NULL == s_part ) { s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, KAPPS_PART_SUBTYPE, NULL); }
    if ( NULL == s_part ) { return false; }
    EXT_RAM_BSS_ATTR static kapps_dir_t a, b;
    bool const oka = (ESP_OK == esp_partition_read(s_part, KAPPS_DIR_A_OFF, &a, sizeof(a)))
                  && (KAPPS_DIR_COPY_OK == kapps_dir_classify(&a, dir_crc(&a), (uint32_t)s_part->size));
    bool const okb = (ESP_OK == esp_partition_read(s_part, KAPPS_DIR_B_OFF, &b, sizeof(b)))
                  && (KAPPS_DIR_COPY_OK == kapps_dir_classify(&b, dir_crc(&b), (uint32_t)s_part->size));
    memset(&s_dir, 0, sizeof(s_dir));
    s_dir_at = KAPPS_DIR_B_OFF;          /* so the first commit goes to A */
    if ( oka && (!okb || (a.gen >= b.gen)) ) { s_dir = a; s_dir_at = KAPPS_DIR_A_OFF; }
    else if ( okb ) { s_dir = b; s_dir_at = KAPPS_DIR_B_OFF; }
    s_dir_ok = true;
    return true;
}

static bool dir_commit(kapps_dir_t *d)
{
    if ( !kapps_dir_sane(d, (uint32_t)s_part->size) ) { return false; }
    d->magic = KAPPS_DIR_MAGIC;
    d->version = KAPPS_FMT_VERSION;
    d->gen = s_dir.gen + 1U;
    d->crc = dir_crc(d);
    uint32_t const at = (KAPPS_DIR_A_OFF == s_dir_at) ? KAPPS_DIR_B_OFF : KAPPS_DIR_A_OFF;
    size_t const body = offsetof(kapps_dir_t, version);
    if ( (ESP_OK != esp_partition_erase_range(s_part, at, SECTOR))
         || (ESP_OK != esp_partition_write(s_part, at + body, (const uint8_t *)d + body, sizeof(*d) - body))
         || (ESP_OK != esp_partition_write(s_part, at, &d->magic, sizeof(d->magic))) )
    {
        return false;
    }
    s_dir = *d;
    s_dir_at = at;
    return true;
}

/* ---- /data/apps.txt: "seq id name" ---- */

typedef struct { uint32_t seq; char id[24]; char name[24]; } meta_t;

static int meta_read(meta_t *m, int max)
{
    FILE *const f = fopen(META, "r");
    int n = 0;
    if ( NULL == f ) { return 0; }
    char line[96];
    while ( (n < max) && fgets(line, sizeof(line), f) )
    {
        unsigned long seq = 0;
        if ( 3 == sscanf(line, "%lu %23s %23[^\n]", &seq, m[n].id, m[n].name) ) { m[n].seq = (uint32_t)seq; ++n; }
    }
    (void)fclose(f);
    return n;
}

static void meta_write(const meta_t *m, int n)
{
    FILE *const f = fopen(META, "w");
    if ( NULL == f ) { return; }
    for ( int i = 0; i < n; ++i ) { fprintf(f, "%lu %s %s\n", (unsigned long)m[i].seq, m[i].id, m[i].name); }
    (void)fclose(f);
}

/** @brief The directory entry of app @p id, -1 when it is not installed. */
static int entry_of(const char *id)
{
    meta_t m[APPS_MAX];
    int const n = meta_read(m, APPS_MAX);
    for ( int k = 0; k < n; ++k )
    {
        if ( 0 != strcmp(m[k].id, id) ) { continue; }
        for ( uint32_t i = 0; i < s_dir.count; ++i )
        {
            if ( (s_dir.e[i].flags & KAPPS_ENTRY_VALID) && (s_dir.e[i].seq == m[k].seq) ) { return (int)i; }
        }
    }
    return -1;
}

static bool lib_has(const char *id);

/* ---- the catalog ---- */

static uint16_t hex565(const char *h)
{
    unsigned r = 0xE8, g = 0xEA, b = 0xED;
    if ( '#' == h[0] ) { (void)sscanf(h + 1, "%2x%2x%2x", &r, &g, &b); }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void parse_index(char *txt)
{
    int n = 0;
    for ( char *line = strtok(txt, "\n"); line && (n < APPS_MAX); line = strtok(NULL, "\n") )
    {
        char *f[7] = { 0 };
        int k = 0;
        for ( char *p = line; p && (k < 7); ++k )
        {
            f[k] = p;
            p = strchr(p, '|');
            if ( p ) { *p++ = '\0'; }
        }
        if ( (k < 4) || (64 != strlen(f[3])) ) { continue; }
        apps_item_t *const it = &s_cat[n++];
        memset(it, 0, sizeof(*it));
        (void)snprintf(it->id, sizeof(it->id), "%s", f[0]);
        (void)snprintf(it->name, sizeof(it->name), "%s", f[1]);
        it->size = (uint32_t)strtoul(f[2], NULL, 10);
        (void)snprintf(it->sha, sizeof(it->sha), "%s", f[3]);
        it->color = (k > 4) ? hex565(f[4]) : 0xEF5D;
        if ( k > 5 ) { (void)snprintf(it->desc, sizeof(it->desc), "%s", f[5]); }
        it->pad = (k > 6) && (NULL != strstr(f[6], "pad"));
    }
    s_ncat = n;
}

static void index_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t const http = { .url = s_url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000, .buffer_size = 2048 };
    esp_http_client_handle_t c = esp_http_client_init(&http);
    EXT_RAM_BSS_ATTR static char buf[4096];
    int got = 0, status = 0;
    esp_err_t e = ESP_FAIL;
    s_err[0] = '\0';
    if ( c && (ESP_OK == (e = esp_http_client_open(c, 0))) )
    {
        (void)esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int n;
        while ( (got < (int)sizeof(buf) - 1) && ((n = esp_http_client_read(c, buf + got, (int)sizeof(buf) - 1 - got)) > 0) ) { got += n; }
    }
    if ( c ) { (void)esp_http_client_close(c); (void)esp_http_client_cleanup(c); }
    if ( (200 == status) && (got > 0) )
    {
        buf[got] = '\0';
        xSemaphoreTake(s_lock, portMAX_DELAY);
        parse_index(buf);
        xSemaphoreGive(s_lock);
        kk_link_send("store ok %d bytes, %d apps", got, s_ncat);
    }
    else
    {
        if ( ESP_OK != e ) { (void)snprintf(s_err, sizeof(s_err), "store: %s", esp_err_to_name(e)); }
        else { (void)snprintf(s_err, sizeof(s_err), "store: HTTP %d, %d bytes", status, got); }
        ESP_LOGW(TAG, "%s", s_err);
        kk_link_send("store fail %s (internal free %u)", s_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    s_loading = false;
    vTaskDelete(NULL);
}

void apps_refresh(const char *url)
{
    if ( NULL == s_lock ) { s_lock = xSemaphoreCreateMutex(); }
    if ( s_loading || (s_pct >= 0) ) { return; }
    (void)snprintf(s_url, sizeof(s_url), "%s", url);
    s_loading = true;
    if ( pdPASS != xTaskCreate(index_task, "appidx", 8192, NULL, 3, NULL) ) { s_loading = false; }
}

bool apps_loading(void) { return s_loading; }
int apps_progress(void) { return s_pct; }
const char *apps_error(void) { return s_err; }

int apps_list(apps_item_t *out, int max)
{
    if ( NULL == s_lock ) { s_lock = xSemaphoreCreateMutex(); }
    if ( !s_dir_ok ) { (void)dir_load(); }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for ( int i = 0; (i < s_ncat) && (n < max); ++i )
    {
        out[n] = s_cat[i];
        out[n].installed = (entry_of(s_cat[i].id) >= 0) || lib_has(s_cat[i].id);
        ++n;
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ---- install ---- */

/** @brief A free, aligned place for @p size bytes: after the last image, or the first gap. */
static bool place(uint32_t size, uint32_t *at)
{
    uint32_t const need = (size + KAPPS_ALIGN - 1U) & ~(KAPPS_ALIGN - 1U);
    uint32_t cand = KAPPS_DATA_OFF;
    for ( int pass = 0; pass < (int)KAPPS_MAX_ENTRIES + 1; ++pass )
    {
        bool moved = false;
        for ( uint32_t i = 0; i < s_dir.count; ++i )
        {
            kapps_entry_t const *const e = &s_dir.e[i];
            if ( !(e->flags & KAPPS_ENTRY_VALID) ) { continue; }
            uint32_t const end = (e->offset + e->size + KAPPS_ALIGN - 1U) & ~(KAPPS_ALIGN - 1U);
            if ( (cand < end) && (e->offset < cand + need) ) { cand = end; moved = true; }
        }
        if ( !moved ) { break; }
    }
    if ( cand + need > s_part->size ) { return false; }
    *at = cand;
    return true;
}

static char s_base[160];

/* ---- the library on the microSD: /sdcard/apps/<id>.bin and <id>.sha ----
 * With a card, Get only downloads to the card; Open copies the app into the
 * flash region when it is not there yet, making room by taking out the apps
 * placed longest ago (they stay on the card). Without a card, Get writes the
 * region directly, as before. */
#define LIB "/sdcard/apps"

static bool lib_ok(void)
{
    struct stat st;
    if ( 0 != stat("/sdcard", &st) ) { return false; }
    if ( 0 != stat(LIB, &st) ) { (void)mkdir(LIB, 0775); }
    return 0 == stat(LIB, &st);
}

static void lib_path(char *out, size_t cap, const char *id, const char *ext) { (void)snprintf(out, cap, LIB "/%s.%s", id, ext); }

static bool lib_has(const char *id)
{
    char p[64];
    struct stat st;
    lib_path(p, sizeof(p), id, "bin");
    return 0 == stat(p, &st);
}

/* A reader: HTTP or a file. */
typedef struct { esp_http_client_handle_t http; FILE *f; } src_t;

static int src_read(src_t *s, uint8_t *buf, int cap)
{
    if ( s->http ) { return esp_http_client_read(s->http, (char *)buf, cap); }
    return (int)fread(buf, 1, (size_t)cap, s->f);
}

static void hex_of(psa_hash_operation_t *sha, char hex[65])
{
    uint8_t d[32];
    size_t dl = 0;
    (void)psa_hash_finish(sha, d, sizeof(d), &dl);
    for ( int i = 0; i < 32; ++i ) { (void)snprintf(&hex[i * 2], 3, "%02x", d[i]); }
}

/** @brief Copy @p size bytes from @p s into the region at @p at, checking @p sha. */
static const char *to_flash(src_t *s, uint32_t at, uint32_t size, const char *sha)
{
    EXT_RAM_BSS_ATTR static uint8_t buf[4096];
    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    (void)psa_crypto_init();
    (void)psa_hash_setup(&h, PSA_ALG_SHA_256);
    const char *why = NULL;
    uint32_t done = 0, erased = 0;
    while ( (NULL == why) && (done < size) )
    {
        int const n = src_read(s, buf, sizeof(buf));
        if ( n <= 0 ) { why = "read"; break; }
        while ( (NULL == why) && (erased < done + (uint32_t)n) )
        {
            if ( ESP_OK != esp_partition_erase_range(s_part, at + erased, KAPPS_ALIGN) ) { why = "erase"; }
            erased += KAPPS_ALIGN;
        }
        if ( why ) { break; }
        if ( ESP_OK != esp_partition_write(s_part, at + done, buf, (size_t)n) ) { why = "write"; break; }
        (void)psa_hash_update(&h, buf, (size_t)n);
        done += (uint32_t)n;
        s_pct = (int)(((uint64_t)done * 100U) / size);
    }
    if ( NULL == why )
    {
        char hex[65];
        hex_of(&h, hex);
        if ( 0 != strcmp(hex, sha) ) { why = "checksum"; }
    }
    (void)psa_hash_abort(&h);
    return why;
}

/** @brief A directory entry and the id/name for the app now at @p at. */
static const char *commit_entry(uint32_t at, uint32_t size, const char *id, const char *name)
{
    EXT_RAM_BSS_ATTR static kapps_dir_t d;
    d = s_dir;
    uint32_t seq = 1, slot = d.count;
    for ( uint32_t i = 0; i < d.count; ++i )
    {
        if ( d.e[i].seq >= seq ) { seq = d.e[i].seq + 1U; }
        if ( !(d.e[i].flags & KAPPS_ENTRY_VALID) && (slot == d.count) ) { slot = i; }
    }
    if ( slot >= KAPPS_MAX_ENTRIES ) { return "too many apps"; }
    if ( slot == d.count ) { d.count++; }
    d.e[slot] = (kapps_entry_t){ .offset = at, .size = size, .seq = seq, .flags = KAPPS_ENTRY_VALID };
    if ( !dir_commit(&d) ) { return "directory"; }
    meta_t m[APPS_MAX];
    int n = meta_read(m, APPS_MAX - 1);
    m[n].seq = seq;
    (void)snprintf(m[n].id, sizeof(m[n].id), "%s", id);
    (void)snprintf(m[n].name, sizeof(m[n].name), "%s", name);
    meta_write(m, n + 1);
    return NULL;
}

/** @brief Take entry @p e out of the directory (the app stays on the card). */
static bool drop_entry(int e)
{
    EXT_RAM_BSS_ATTR static kapps_dir_t d;
    d = s_dir;
    uint32_t const seq = d.e[e].seq;
    d.e[e].flags &= ~KAPPS_ENTRY_VALID;
    if ( !dir_commit(&d) ) { return false; }
    meta_t m[APPS_MAX];
    int const n = meta_read(m, APPS_MAX);
    int k = 0;
    for ( int i = 0; i < n; ++i ) { if ( m[i].seq != seq ) { m[k++] = m[i]; } }
    meta_write(m, k);
    return true;
}

/** @brief A place for @p size bytes, taking out the oldest placed apps that the card still holds. */
static bool place_evicting(uint32_t size, uint32_t *at)
{
    while ( !place(size, at) )
    {
        meta_t m[APPS_MAX];
        int const n = meta_read(m, APPS_MAX);
        int victim = -1;
        uint32_t oldest = UINT32_MAX;
        for ( uint32_t i = 0; i < s_dir.count; ++i )
        {
            if ( !(s_dir.e[i].flags & KAPPS_ENTRY_VALID) || (s_dir.e[i].seq >= oldest) ) { continue; }
            for ( int k = 0; k < n; ++k )
            {
                if ( (m[k].seq == s_dir.e[i].seq) && lib_has(m[k].id) ) { victim = (int)i; oldest = s_dir.e[i].seq; }
            }
        }
        if ( (victim < 0) || !drop_entry(victim) ) { return false; }
        ESP_LOGI(TAG, "made room: an app placed earlier stays on the card");
    }
    return true;
}

static void install_task(void *arg)
{
    (void)arg;
    apps_item_t it;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    it = s_cat[s_want];
    xSemaphoreGive(s_lock);
    const char *why = NULL;
    uint32_t at = 0;
    char url[200];
    (void)snprintf(url, sizeof(url), "%s%s.bin?v=%.12s", s_base, it.id, it.sha);   /* a new build is a new URL for the CDN */
    bool const card = lib_ok();
    if ( !dir_load() ) { why = "no apps region"; }
    else if ( (entry_of(it.id) >= 0) || (card && lib_has(it.id)) ) { why = "installed already"; }
    else if ( !card && !place(it.size, &at) ) { why = "no room (put in a microSD)"; }
    esp_http_client_config_t const http = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size = 4096 };
    src_t src = { 0 };
    if ( NULL == why )
    {
        src.http = esp_http_client_init(&http);
        if ( (NULL == src.http) || (ESP_OK != esp_http_client_open(src.http, 0)) ) { why = "download"; }
        else if ( esp_http_client_fetch_headers(src.http) != (int64_t)it.size ) { why = "size"; }
    }
    if ( (NULL == why) && card )
    {
        /* to the card: <id>.tmp, checked, then renamed */
        char tmp[64], fin[64], shp[64];
        lib_path(tmp, sizeof(tmp), it.id, "tmp");
        lib_path(fin, sizeof(fin), it.id, "bin");
        lib_path(shp, sizeof(shp), it.id, "sha");
        FILE *const f = fopen(tmp, "wb");
        EXT_RAM_BSS_ATTR static uint8_t buf[8192];
        psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
        (void)psa_crypto_init();
        (void)psa_hash_setup(&h, PSA_ALG_SHA_256);
        uint32_t done = 0;
        if ( NULL == f ) { why = "card write"; }
        while ( (NULL == why) && (done < it.size) )
        {
            int const n = src_read(&src, buf, sizeof(buf));
            if ( n <= 0 ) { why = "download"; break; }
            if ( fwrite(buf, 1, (size_t)n, f) != (size_t)n ) { why = "card write"; break; }
            (void)psa_hash_update(&h, buf, (size_t)n);
            done += (uint32_t)n;
            s_pct = (int)(((uint64_t)done * 100U) / it.size);
        }
        if ( f ) { (void)fclose(f); }
        if ( NULL == why )
        {
            char hex[65];
            hex_of(&h, hex);
            if ( 0 != strcmp(hex, it.sha) ) { why = "checksum"; }
        }
        (void)psa_hash_abort(&h);
        if ( NULL == why )
        {
            (void)remove(fin);
            if ( 0 != rename(tmp, fin) ) { why = "card write"; }
            FILE *const sf = fopen(shp, "w");
            if ( sf ) { fprintf(sf, "%s %lu %s\n", it.sha, (unsigned long)it.size, it.name); (void)fclose(sf); }
        }
        if ( why ) { (void)remove(tmp); }
    }
    else if ( NULL == why )
    {
        why = to_flash(&src, at, it.size, it.sha);
        if ( NULL == why ) { why = commit_entry(at, it.size, it.id, it.name); }
    }
    if ( src.http ) { (void)esp_http_client_close(src.http); (void)esp_http_client_cleanup(src.http); }
    if ( why )
    {
        ESP_LOGW(TAG, "install %s failed: %s", it.id, why);
        (void)snprintf(s_err, sizeof(s_err), "%s: %s", it.name, why);
        kk_link_send("app fail %s %s", it.id, why);
    }
    else
    {
        ESP_LOGI(TAG, "installed %s %s", it.id, card ? "on the card" : "in flash");
        kk_link_send("app installed %s%s", it.id, card ? " (card)" : "");
    }
    s_pct = -1;
    vTaskDelete(NULL);
}

bool apps_install(int i, const char *base_url)
{
    if ( (s_pct >= 0) || s_loading || (i < 0) || (i >= s_ncat) ) { return false; }
    (void)snprintf(s_base, sizeof(s_base), "%s", base_url);
    s_want = i;
    s_err[0] = '\0';
    s_pct = 0;
    if ( pdPASS != xTaskCreate(install_task, "appget", 8192, NULL, 4, NULL) ) { s_pct = -1; return false; }
    return true;
}

static char s_open_id[24];

static void boot_into(const char *id)
{
    int const e = entry_of(id);
    if ( e < 0 ) { return; }
    kapps_req_t req = { .magic = KAPPS_REQ_MAGIC, .offset = s_dir.e[e].offset, .size = s_dir.e[e].size, .state = KAPPS_REQ_STATE_NEW };
    req.crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&req, offsetof(kapps_req_t, crc));
    if ( (ESP_OK != esp_partition_erase_range(s_part, KAPPS_REQ_OFF, SECTOR))
         || (ESP_OK != esp_partition_write(s_part, KAPPS_REQ_OFF, &req, sizeof(req))) )
    {
        (void)snprintf(s_err, sizeof(s_err), "could not start %s", id);
        return;
    }
    bool pad = false;
    for ( int i = 0; i < s_ncat; ++i ) { if ( 0 == strcmp(s_cat[i].id, id) ) { pad = s_cat[i].pad; } }
    nvs_handle_t h;
    if ( ESP_OK == nvs_open("kwatch", NVS_READWRITE, &h) )
    {
        (void)nvs_set_u8(h, "pad", pad ? 1U : 0U);   /* read by the SDK's kpad_s3.c in the app */
        (void)nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "opening %s%s: restart into it", id, pad ? " (virtual pad)" : "");
    kk_link_send("app open %s", id);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* Open an app the card holds but the region does not: copy it in first. */
static void stage_task(void *arg)
{
    (void)arg;
    char bin[64], shp[64], sha[65] = "", name[24] = "";
    unsigned long size = 0;
    lib_path(bin, sizeof(bin), s_open_id, "bin");
    lib_path(shp, sizeof(shp), s_open_id, "sha");
    const char *why = NULL;
    FILE *const sf = fopen(shp, "r");
    if ( (NULL == sf) || (2 > fscanf(sf, "%64s %lu %23[^\n]", sha, &size, name)) ) { why = "card: no checksum"; }
    if ( sf ) { (void)fclose(sf); }
    uint32_t at = 0;
    if ( (NULL == why) && !place_evicting((uint32_t)size, &at) ) { why = "no room"; }
    src_t src = { 0 };
    if ( NULL == why ) { src.f = fopen(bin, "rb"); if ( NULL == src.f ) { why = "card read"; } }
    if ( NULL == why ) { why = to_flash(&src, at, (uint32_t)size, sha); }
    if ( src.f ) { (void)fclose(src.f); }
    if ( NULL == why ) { why = commit_entry(at, (uint32_t)size, s_open_id, name[0] ? name : s_open_id); }
    s_pct = -1;
    if ( NULL == why ) { boot_into(s_open_id); }
    ESP_LOGW(TAG, "open %s failed: %s", s_open_id, why ? why : "boot");
    (void)snprintf(s_err, sizeof(s_err), "%s: %s", s_open_id, why ? why : "did not start");
    vTaskDelete(NULL);
}

bool apps_open(const char *id)
{
    if ( !dir_load() || (s_pct >= 0) ) { return false; }
    if ( entry_of(id) >= 0 ) { boot_into(id); return false; }
    if ( !lib_ok() || !lib_has(id) ) { return false; }
    (void)snprintf(s_open_id, sizeof(s_open_id), "%s", id);
    s_err[0] = '\0';
    s_pct = 0;
    if ( pdPASS != xTaskCreate(stage_task, "appstage", 8192, NULL, 4, NULL) ) { s_pct = -1; return false; }
    return true;
}

bool apps_remove(const char *id)
{
    if ( !dir_load() ) { return false; }
    int const e = entry_of(id);
    bool ok = (e < 0) || drop_entry(e);
    if ( lib_ok() )
    {
        char p[64];
        lib_path(p, sizeof(p), id, "bin");
        (void)remove(p);
        lib_path(p, sizeof(p), id, "sha");
        (void)remove(p);
    }
    return ok;
}

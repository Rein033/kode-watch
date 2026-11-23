/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fs.c
 * @brief Files on the microSD (/sdcard) and the flash's /data: for the Files app
 *        and for the hub's file manager.
 *
 * The hub's lines (and the answers):
 *   fs ls <dir>          fs ent <d|f> <size> <name> ... then fs end ls <n>|fail
 *   fs rm <path>         fs end rm ok|fail     (a folder only when empty)
 *   fs mkdir <path>      fs end mkdir ok|fail
 *   fs get <path>        fs size <n>, binary "KFS1"+data frames, fs end get ok|fail
 *   fs put <n> <path>    fs ack <bytes> after each binary "KFS2"+data frame, fs end put ok|fail
 * Only paths under /sdcard or /data, without "..".
 */
#include "fs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kk.h"
#include "net.h"

#define TAG "fs"
#define CHUNK 3072

bool fs_path_ok(const char *p)
{
    if ( (NULL == p) || (strlen(p) >= FS_PATH) ) { return false; }
    if ( strstr(p, "..") ) { return false; }
    return (0 == strcmp(p, "/sdcard")) || (0 == strncmp(p, "/sdcard/", 8))
        || (0 == strcmp(p, "/data")) || (0 == strncmp(p, "/data/", 6));
}

static int cmp(const void *a, const void *b)
{
    const fs_ent_t *x = a, *y = b;
    if ( x->dir != y->dir ) { return x->dir ? -1 : 1; }
    return strcasecmp(x->name, y->name);
}

int fs_list(const char *dir, fs_ent_t *out, int max)
{
    if ( !fs_path_ok(dir) ) { return -1; }
    DIR *d = opendir(dir);
    if ( NULL == d ) { return -1; }
    int n = 0;
    struct dirent *e;
    char p[FS_PATH + 64];
    while ( (n < max) && (NULL != (e = readdir(d))) )
    {
        if ( '.' == e->d_name[0] && ('\0' == e->d_name[1] || ('.' == e->d_name[1] && '\0' == e->d_name[2])) ) { continue; }
        fs_ent_t *const f = &out[n];
        (void)snprintf(f->name, sizeof(f->name), "%s", e->d_name);
        (void)snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if ( 0 == stat(p, &st) ) { f->dir = S_ISDIR(st.st_mode); f->size = f->dir ? 0 : (uint32_t)st.st_size; }
        else { f->dir = (DT_DIR == e->d_type); f->size = 0; }
        ++n;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), cmp);
    return n;
}

bool fs_remove(const char *path)
{
    if ( !fs_path_ok(path) || (0 == strcmp(path, "/sdcard")) || (0 == strcmp(path, "/data")) ) { return false; }
    struct stat st;
    if ( 0 != stat(path, &st) ) { return false; }
    return 0 == (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path));
}

/* ---- the hub's file manager ---- */

static char s_get[FS_PATH];
static volatile bool s_get_busy;

static void get_task(void *arg)
{
    (void)arg;
    bool ok = false;
    FILE *f = fopen(s_get, "rb");
    uint8_t *const b = heap_caps_malloc(CHUNK + 4, MALLOC_CAP_SPIRAM);
    if ( f && b )
    {
        (void)fseek(f, 0, SEEK_END);
        long const size = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
        kk_link_send("fs size %ld", size);
        memcpy(b, "KFS1", 4);
        ok = true;
        for ( ;; )
        {
            size_t const n = fread(b + 4, 1, CHUNK, f);
            if ( 0 == n ) { break; }
            int tries = 0;
            while ( !net_send_bin(b, n + 4) )
            {
                if ( ++tries > 5 ) { ok = false; break; }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if ( !ok ) { break; }
        }
        ok = ok && !ferror(f);
    }
    if ( f ) { fclose(f); }
    heap_caps_free(b);
    kk_link_send("fs end get %s", ok ? "ok" : "fail");
    s_get_busy = false;
    vTaskDelete(NULL);
}

/* an upload: written to <path>.part, renamed when complete */
static FILE *s_put;
static char s_put_path[FS_PATH];
static uint32_t s_put_left, s_put_done;
static int64_t s_put_at;

/* answers from the socket's own task wait for fs_tick (no send from inside its event) */
static volatile uint32_t s_ack = UINT32_MAX;
static volatile int s_end;   /* 0 none, 1 ok, 2 fail */

static void put_end(bool ok)
{
    char part[FS_PATH + 8];
    (void)snprintf(part, sizeof(part), "%s.part", s_put_path);
    if ( s_put ) { ok = (0 == fclose(s_put)) && ok; s_put = NULL; }
    if ( ok )
    {
        (void)unlink(s_put_path);
        ok = (0 == rename(part, s_put_path));
    }
    if ( !ok ) { (void)unlink(part); }
    s_end = ok ? 1 : 2;
}

static void on_bin(const char *d, size_t n)
{
    if ( (n < 4) || (0 != memcmp(d, "KFS2", 4)) || (NULL == s_put) ) { return; }
    size_t k = n - 4;
    if ( k > s_put_left ) { k = s_put_left; }
    if ( k && (fwrite(d + 4, 1, k, s_put) != k) ) { put_end(false); return; }
    s_put_left -= (uint32_t)k;
    s_put_done += (uint32_t)k;
    s_put_at = esp_timer_get_time();
    if ( 0 == s_put_left ) { put_end(true); }
    else { s_ack = s_put_done; }
}

void fs_init(void) { net_set_bin_rx(on_bin); }

void fs_tick(void)
{
    uint32_t const a = s_ack;
    if ( UINT32_MAX != a ) { s_ack = UINT32_MAX; kk_link_send("fs ack %lu", (unsigned long)a); }
    int const e = s_end;
    if ( e ) { s_end = 0; kk_link_send("fs end put %s", (1 == e) ? "ok" : "fail"); }
    /* an upload that stopped coming */
    if ( s_put && ((esp_timer_get_time() - s_put_at) > 20000000LL) ) { ESP_LOGW(TAG, "upload timed out"); put_end(false); }
}

bool fs_on_line(const char *m)
{
    if ( 0 != strncmp(m, "fs ", 3) ) { return false; }
    m += 3;
    if ( 0 == strncmp(m, "ls ", 3) )
    {
        const char *const dir = m + 3;
        enum { MAXE = 200 };
        fs_ent_t *const e = heap_caps_malloc(sizeof(fs_ent_t) * MAXE, MALLOC_CAP_SPIRAM);
        int const n = e ? fs_list(dir, e, MAXE) : -1;
        for ( int i = 0; i < n; ++i ) { kk_link_send("fs ent %c %lu %s", e[i].dir ? 'd' : 'f', (unsigned long)e[i].size, e[i].name); }
        if ( n >= 0 ) { kk_link_send("fs end ls %d", n); }
        else { kk_link_send("fs end ls fail"); }
        heap_caps_free(e);
    }
    else if ( 0 == strncmp(m, "rm ", 3) ) { kk_link_send("fs end rm %s", fs_remove(m + 3) ? "ok" : "fail"); }
    else if ( 0 == strncmp(m, "mkdir ", 6) )
    {
        bool const ok = fs_path_ok(m + 6) && (0 == mkdir(m + 6, 0775));
        kk_link_send("fs end mkdir %s", ok ? "ok" : "fail");
    }
    else if ( 0 == strncmp(m, "get ", 4) )
    {
        if ( s_get_busy || !fs_path_ok(m + 4) ) { kk_link_send("fs end get fail"); return true; }
        (void)snprintf(s_get, sizeof(s_get), "%s", m + 4);
        s_get_busy = true;
        if ( pdPASS != xTaskCreate(get_task, "fsget", 4096, NULL, 2, NULL) ) { s_get_busy = false; kk_link_send("fs end get fail"); }
    }
    else if ( 0 == strncmp(m, "put ", 4) )
    {
        unsigned long size = 0;
        int k = 0;
        if ( s_put ) { put_end(false); }
        if ( (1 != sscanf(m + 4, "%lu %n", &size, &k)) || !k || !fs_path_ok(m + 4 + k) ) { kk_link_send("fs end put fail"); return true; }
        (void)snprintf(s_put_path, sizeof(s_put_path), "%s", m + 4 + k);
        char part[FS_PATH + 8];
        (void)snprintf(part, sizeof(part), "%s.part", s_put_path);
        s_put = fopen(part, "wb");
        if ( NULL == s_put ) { kk_link_send("fs end put fail"); return true; }
        s_put_left = (uint32_t)size;
        s_put_done = 0;
        s_put_at = esp_timer_get_time();
        s_end = 0;
        s_ack = UINT32_MAX;
        if ( 0 == size ) { put_end(true); }
        else { kk_link_send("fs ack 0"); }
    }
    return true;
}

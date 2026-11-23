/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rlog.c
 * @brief The console's log, also to the hub ("log <line>"), so the watch can be
 *        followed without a cable. Lines are queued from any task and sent by a
 *        task of their own while the hub is up; when the queue is full, new lines drop.
 */
#include "rlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

#define RL_N 96
#define RL_LEN 168

EXT_RAM_BSS_ATTR static char s_q[RL_N][RL_LEN];
static unsigned s_h, s_t;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static int hook(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int const r = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    char line[RL_LEN];
    (void)vsnprintf(line, sizeof(line) - 4, fmt, cp);
    va_end(cp);
    /* without the colour codes and the line end */
    char out[RL_LEN];
    int o = 0;
    for ( const char *p = line; *p && (o < (int)sizeof(out) - 1); ++p )
    {
        if ( '\x1b' == *p ) { while ( *p && ('m' != *p) ) { ++p; } if ( !*p ) { break; } continue; }
        if ( ('\n' == *p) || ('\r' == *p) ) { continue; }
        out[o++] = *p;
    }
    out[o] = '\0';
    if ( o > 0 )
    {
        taskENTER_CRITICAL(&s_mux);
        if ( (s_h - s_t) < RL_N ) { memcpy(s_q[s_h % RL_N], "log ", 4); memcpy(s_q[s_h % RL_N] + 4, out, (size_t)o + 1); ++s_h; }
        taskEXIT_CRITICAL(&s_mux);
    }
    return r;
}

static void rlog_task(void *arg)
{
    (void)arg;
    char m[RL_LEN + 4];
    for ( ;; )
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        int sent = 0;
        while ( net_hub_up() && (sent < 8) )
        {
            bool have = false;
            taskENTER_CRITICAL(&s_mux);
            if ( s_h != s_t ) { memcpy(m, s_q[s_t % RL_N], sizeof(s_q[0])); ++s_t; have = true; }
            taskEXIT_CRITICAL(&s_mux);
            if ( !have ) { break; }
            (void)net_send(m, strlen(m));
            ++sent;
        }
    }
}

void rlog_init(void)
{
    s_prev = esp_log_set_vprintf(hook);
    (void)xTaskCreate(rlog_task, "rlog", 3072, NULL, 1, NULL);
}

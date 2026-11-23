/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file crash.c
 * @brief Why the watch started: the reset reason, and after a panic the core
 *        dump's summary (task, PC, backtrace) from the coredump partition.
 *        The OS sends it to the hub once ("boot ..." / "crash ..."), then the
 *        dump is erased. Decode a PC on CT 123 with the version's ELF:
 *          xtensa-esp32s3-elf-addr2line -pfiaC -e /opt/kode/work/watch/elf/kw-<ver>.elf 0x...
 */
#include "crash.h"

#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

static char s_rep[240];
static bool s_dump;

static const char *reason(esp_reset_reason_t r)
{
    switch ( r )
    {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_USB: return "usb";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

void crash_init(void)
{
    const char *const why = reason(esp_reset_reason());
    (void)snprintf(s_rep, sizeof(s_rep), "boot %s", why);
    if ( ESP_OK != esp_core_dump_image_check() ) { ESP_LOGI("crash", "%s", s_rep); return; }
    esp_core_dump_summary_t *const sm = heap_caps_calloc(1, sizeof(*sm), MALLOC_CAP_SPIRAM);
    if ( sm && (ESP_OK == esp_core_dump_get_summary(sm)) )
    {
        int n = snprintf(s_rep, sizeof(s_rep), "crash %s task=%.16s pc=0x%08lx bt", why, sm->exc_task, (unsigned long)sm->exc_pc);
        for ( uint32_t i = 0; (i < sm->exc_bt_info.depth) && (i < 10) && (n > 0) && (n < (int)sizeof(s_rep) - 12); ++i )
        {
            n += snprintf(s_rep + n, sizeof(s_rep) - (size_t)n, " 0x%08lx", (unsigned long)sm->exc_bt_info.bt[i]);
        }
        s_dump = true;
    }
    heap_caps_free(sm);
    ESP_LOGE("crash", "%s", s_rep);
}

const char *crash_report(void) { return s_rep; }

void crash_sent(void)
{
    if ( s_dump ) { (void)esp_core_dump_image_erase(); s_dump = false; }
}

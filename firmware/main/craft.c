/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file craft.c
 * @brief See craft.h. bareiron's loop busy-waits and yields once in a while,
 *        so it runs on core 1 at the lowest priority: the panel's flush and
 *        the touch task there preempt it, and the OS's own loop on core 0 is
 *        not touched.
 */
#include "craft.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "globals.h"
#include "net.h"

int bareiron_run(void);

static volatile craft_state_t s_state = CRAFT_OFF;

/* bareiron's exit(): the server stops, the watch does not. */
void bareiron_exit(int code)
{
    ESP_LOGW("craft", "server stopped (%d)", code);
    s_state = CRAFT_FAILED;
    vTaskDelete(NULL);
    for ( ;; ) {}
}

static void craft_task(void *arg)
{
    (void)arg;
    (void)bareiron_run();
    s_state = CRAFT_FAILED;
    vTaskDelete(NULL);
}

bool craft_start(void)
{
    if ( CRAFT_RUNNING == s_state ) { return true; }
    net_low_latency(true);   /* players move in real time: no Wi-Fi power save */
    s_state = CRAFT_RUNNING;
    if ( pdPASS != xTaskCreatePinnedToCore(craft_task, "craft", 12288, NULL, 1, NULL, 1) )
    {
        s_state = CRAFT_FAILED;
        return false;
    }
    return true;
}

craft_state_t craft_state(void) { return s_state; }

int craft_players(void)
{
    int n = 0;
    for ( int i = 0; i < MAX_PLAYERS; ++i ) { if ( player_data[i].client_fd != -1 ) { ++n; } }
    return (CRAFT_RUNNING == s_state) ? n : 0;
}

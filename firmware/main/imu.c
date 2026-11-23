/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu.c
 * @brief See imu.h. The accelerometer at 62.5 Hz, +-4 g, read by a 50 Hz task.
 *
 * Steps: the magnitude, smoothed, crosses up through 1.12 g after having been
 * under 0.96 g, at most one step per 280 ms and only inside a walk (four such
 * crossings within 4 s; the first ones are counted once the walk is known).
 * Raise: the glass goes from on edge (|z| under 0.45 g, an arm hanging or
 * swinging) to facing you or the sky (|z| over 0.80 g) within 0.8 s, then holds
 * still for 150 ms. Which way up the chip sits does not matter.
 */
#include "imu.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"

#define ADDR 0x6B
static i2c_master_dev_handle_t s_dev;
static bool s_ok;
static volatile uint32_t s_steps;
static volatile bool s_raised;
static volatile float s_act_sum;     /* |mag - slow mean|, summed */
static volatile uint32_t s_act_n;

static bool wr(uint8_t r, uint8_t v)
{
    uint8_t const b[2] = { r, v };
    return ESP_OK == i2c_master_transmit(s_dev, b, 2, 30);
}

static bool rd(uint8_t r, uint8_t *v, size_t n)
{
    return ESP_OK == i2c_master_transmit_receive(s_dev, &r, 1, v, n, 30);
}

static void imu_task(void *arg)
{
    (void)arg;
    float lp = 1.0f;
    bool low = false;
    int64_t last_step = 0;
    int64_t walk_t[4] = { 0 };
    int walk_n = 0, pending = 0;
    bool walking = false;
    int64_t edge_at = 0;
    int64_t still_from = 0;
    bool armed_raise = false;
    for ( ;; )
    {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t b[6];
        if ( !rd(0x35, b, 6) ) { continue; }
        float const ax = (float)(int16_t)(b[0] | (b[1] << 8)) / 8192.0f;   /* +-4 g: 8192 LSB/g */
        float const ay = (float)(int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
        float const az = (float)(int16_t)(b[4] | (b[5] << 8)) / 8192.0f;
        float const mag = sqrtf(ax * ax + ay * ay + az * az);
        int64_t const now = esp_timer_get_time();
        lp += (mag - lp) * 0.35f;
        {
            static float slow = 1.0f;
            slow += (mag - slow) * 0.01f;
            s_act_sum += fabsf(mag - slow);
            ++s_act_n;
        }

        /* steps */
        if ( lp < 0.96f ) { low = true; }
        if ( low && (lp > 1.12f) && ((now - last_step) > 280000) )
        {
            low = false;
            last_step = now;
            walk_t[walk_n % 4] = now;
            ++walk_n;
            if ( walking ) { ++s_steps; }
            else
            {
                ++pending;
                if ( (walk_n >= 4) && ((now - walk_t[(walk_n - 4) % 4]) < 4000000) ) { walking = true; s_steps += (uint32_t)pending; pending = 0; }
            }
        }
        if ( (now - last_step) > 2000000 ) { walking = false; pending = 0; walk_n = 0; }

        /* raise to look */
        float const z = fabsf(az);
        if ( z < 0.45f ) { edge_at = now; armed_raise = true; }
        if ( armed_raise && (z > 0.80f) && ((now - edge_at) < 800000) )
        {
            if ( fabsf(mag - 1.0f) < 0.12f )
            {
                if ( 0 == still_from ) { still_from = now; }
                if ( (now - still_from) > 150000 ) { s_raised = true; armed_raise = false; still_from = 0; }
            }
            else { still_from = 0; }
        }
        else if ( (now - edge_at) >= 800000 ) { armed_raise = false; still_from = 0; }
    }
}

void imu_init(void)
{
    i2c_device_config_t const cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000 };
    if ( ESP_OK != i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev) ) { return; }
    uint8_t who = 0;
    if ( !rd(0x00, &who, 1) || (0x05 != who) )
    {
        ESP_LOGW("imu", "no QMI8658 (who_am_i 0x%02x)", who);
        return;
    }
    s_ok = wr(0x02, 0x60)        /* CTRL1: address auto-increment, little endian */
        && wr(0x03, 0x16)        /* CTRL2: accel +-4 g, ODR 62.5 Hz */
        && wr(0x08, 0x01);       /* CTRL7: accelerometer on */
    if ( s_ok ) { xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 3, NULL, 0); }
    ESP_LOGI("imu", "QMI8658 %s", s_ok ? "up" : "did not start");
}

bool imu_ok(void) { return s_ok; }
uint32_t imu_steps(void) { return s_steps; }

int imu_motion_take(void)
{
    uint32_t const n = s_act_n;
    float const sum = s_act_sum;
    s_act_sum = 0.0f;
    s_act_n = 0;
    if ( (0 == n) || !s_ok ) { return -1; }
    return (int)(sum * 1000.0f / (float)n);
}

bool imu_raised(void)
{
    bool const r = s_raised;
    s_raised = false;
    return r;
}

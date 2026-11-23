/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file board.c
 * @brief Kode-watch board layer, see board.h.
 */
#include "board.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"

#include "kk.h"
#include "net.h"

static const char *TAG = "board";

#define CHUNK_ROWS 50                 /* even, so every window starts on an even row */
#define CHUNK_PX   (KK_W * CHUNK_ROWS)
#define BOOT_GPIO  GPIO_NUM_0

#define AXP_ADDR 0x34
#define RTC_ADDR 0x51

#define LINK_LEN   512
#define LINK_DEPTH 48   /* in PSRAM: the hub sends a burst on connect */

static esp_lcd_panel_handle_t s_panel;
static volatile int s_panel_want = 1, s_panel_sent = 1;   /* the AMOLED on (1) or asleep (0) */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static i2c_master_dev_handle_t s_axp, s_rtc;
static bool s_has_axp, s_has_rtc;

static uint16_t *s_fbs[2];
static uint16_t *s_dma[2];
static SemaphoreHandle_t s_chunk_done;   /* one give per finished colour transfer */
static SemaphoreHandle_t s_frame_idle;   /* the flush task is free */
static QueueHandle_t s_frame_q;
static QueueHandle_t s_link_q;

static volatile bool s_pwr_click;
static volatile int s_bright_want = -1;   /* set by any task, sent by the flush task between frames */
static int s_bright_sent = -1;
static volatile bool s_tdown;
static volatile int s_tx, s_ty;
static volatile int s_batt = -1;
static volatile bool s_vbus, s_chg;

/* ---- I2C helpers ---- */

static bool reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    return ESP_OK == i2c_master_transmit_receive(d, &reg, 1, buf, n, 50);
}

static bool reg_write(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[16];
    if ( n > sizeof(tmp) - 1 ) { return false; }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, n);
    return ESP_OK == i2c_master_transmit(d, tmp, n + 1, 50);
}

static bool reg_set_bits(i2c_master_dev_handle_t d, uint8_t reg, uint8_t bits)
{
    uint8_t v = 0;
    if ( !reg_read(d, reg, &v, 1) ) { return false; }
    v |= bits;
    return reg_write(d, reg, &v, 1);
}

static i2c_master_dev_handle_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr, bool *ok)
{
    *ok = (ESP_OK == i2c_master_probe(bus, addr, 50));
    if ( !*ok )
    {
        ESP_LOGW(TAG, "no I2C device at 0x%02x", addr);
        return NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t h = NULL;
    if ( ESP_OK != i2c_master_bus_add_device(bus, &cfg, &h) ) { *ok = false; }
    return h;
}

/* ---- display ---- */

static bool IRAM_ATTR on_chunk_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io;
    (void)e;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_done, &woken);
    return pdTRUE == woken;
}

static void flush_task(void *arg)
{
    (void)arg;
    const uint16_t *fb = NULL;
    for ( ;; )
    {
        if ( pdTRUE != xQueueReceive(s_frame_q, &fb, portMAX_DELAY) ) { continue; }
        /* The panel's commands share the bus with the frame: only this task talks to it. */
        int const pw = s_panel_want;
        if ( (pw != s_panel_sent) && pw )
        {
            (void)esp_lcd_panel_disp_sleep(s_panel, false);
            vTaskDelay(pdMS_TO_TICKS(5));
            (void)esp_lcd_panel_disp_on_off(s_panel, true);
            s_panel_sent = pw;
        }
        int const want = s_bright_want;
        if ( (want >= 0) && (want != s_bright_sent) )
        {
            (void)bsp_display_brightness_set(want);
            s_bright_sent = want;
        }
        int slot = 0, inflight = 0;
        for ( int y = 0; y < KK_H; y += CHUNK_ROWS )
        {
            int const rows = ((KK_H - y) < CHUNK_ROWS) ? (KK_H - y) : CHUNK_ROWS;
            if ( 2 == inflight )
            {
                if ( pdTRUE != xSemaphoreTake(s_chunk_done, pdMS_TO_TICKS(200)) ) { ESP_LOGW(TAG, "flush: chunk timeout"); }
                --inflight;
            }
            /* The panel wants big-endian RGB565: swap while copying into DMA memory. */
            uint16_t *const d = s_dma[slot];
            const uint16_t *const s = fb + (size_t)y * KK_W;
            int const n = rows * KK_W;
            for ( int i = 0; i < n; ++i ) { d[i] = (uint16_t)((s[i] >> 8) | (s[i] << 8)); }
            if ( ESP_OK == esp_lcd_panel_draw_bitmap(s_panel, 0, y, KK_W, y + rows, d) ) { ++inflight; }
            else { ESP_LOGW(TAG, "flush: draw failed at row %d", y); }
            slot ^= 1;
        }
        while ( inflight > 0 )
        {
            if ( pdTRUE != xSemaphoreTake(s_chunk_done, pdMS_TO_TICKS(200)) ) { ESP_LOGW(TAG, "flush: end timeout"); }
            --inflight;
        }
        xSemaphoreTake(s_chunk_done, 0);   /* drop a late give, so the count stays right */
        if ( (s_panel_want != s_panel_sent) && !s_panel_want )
        {
            (void)esp_lcd_panel_disp_on_off(s_panel, false);
            (void)esp_lcd_panel_disp_sleep(s_panel, true);
            s_panel_sent = 0;
        }
        (void)xSemaphoreGive(s_frame_idle);
    }
}

uint16_t *board_fb(int i) { return s_fbs[i & 1]; }

void board_flush(const uint16_t *fb)
{
    if ( pdTRUE != xSemaphoreTake(s_frame_idle, pdMS_TO_TICKS(500)) )
    {
        ESP_LOGW(TAG, "flush still busy, frame dropped");
        return;
    }
    if ( pdTRUE != xQueueSend(s_frame_q, &fb, 0) ) { (void)xSemaphoreGive(s_frame_idle); }
}

void board_panel(bool on) { s_panel_want = on ? 1 : 0; }

void board_brightness(int pct)
{
    if ( pct < 0 ) { pct = 0; }
    if ( pct > 100 ) { pct = 100; }
    s_bright_want = pct;
}

/* ---- PMU ---- */

static void pmu_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for ( ;; )
    {
        uint8_t st = 0;
        if ( reg_read(s_axp, 0x49, &st, 1) && (st & 0x08U) )
        {
            s_pwr_click = true;
            uint8_t const clr = 0x08U;
            (void)reg_write(s_axp, 0x49, &clr, 1);
        }
        if ( 0 == (tick++ % 20U) )
        {
            uint8_t s1 = 0, s2 = 0, pct = 0;
            if ( reg_read(s_axp, 0x00, &s1, 1) && reg_read(s_axp, 0x01, &s2, 1) )
            {
                s_vbus = (s1 & 0x20U) != 0U;
                s_chg = ((s2 >> 5) & 0x03U) == 0x01U;
                if ( (s1 & 0x08U) && reg_read(s_axp, 0xA4, &pct, 1) ) { s_batt = (pct > 100U) ? 100 : pct; }
                else { s_batt = -1; }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int board_battery(void) { return s_batt; }

void board_power_off(void)
{
    /* AXP2101 common config (0x10) bit 0: soft power off. PWR wakes it again. */
    if ( s_has_axp ) { (void)reg_set_bits(s_axp, 0x10, 0x01); }
}
bool board_power_in(void) { return s_vbus; }
bool board_charging(void) { return s_chg; }

/* ---- RTC (PCF85063) ---- */

static uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10U + (v & 0x0FU)); }
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t const era = (y >= 0 ? y : y - 399) / 400;
    int64_t const yoe = y - era * 400;
    int64_t const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool board_rtc_get(int64_t *local_epoch)
{
    if ( !s_has_rtc ) { return false; }
    uint8_t b[7];
    if ( !reg_read(s_rtc, 0x04, b, sizeof(b)) ) { return false; }
    if ( b[0] & 0x80U ) { return false; }   /* OS flag: the oscillator stopped, time is not valid */
    int const sec = from_bcd(b[0] & 0x7FU), min = from_bcd(b[1] & 0x7FU), hour = from_bcd(b[2] & 0x3FU);
    int const day = from_bcd(b[3] & 0x3FU), mon = from_bcd(b[5] & 0x1FU), year = 2000 + from_bcd(b[6]);
    if ( (mon < 1) || (mon > 12) || (day < 1) || (year < 2024) ) { return false; }
    *local_epoch = days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    return true;
}

void board_rtc_set(int64_t local_epoch)
{
    if ( !s_has_rtc ) { return; }
    time_t const t = (time_t)local_epoch;
    struct tm tm;
    (void)gmtime_r(&t, &tm);
    uint8_t const b[7] = {
        to_bcd(tm.tm_sec), to_bcd(tm.tm_min), to_bcd(tm.tm_hour), to_bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday, to_bcd(tm.tm_mon + 1), to_bcd(tm.tm_year % 100),
    };
    if ( !reg_write(s_rtc, 0x04, b, sizeof(b)) ) { ESP_LOGW(TAG, "RTC write failed"); }
}

/* ---- buttons and touch ---- */

uint32_t board_buttons(void)
{
    uint32_t m = 0U;
    if ( 0 == gpio_get_level(BOOT_GPIO) ) { m |= KK_TOP; }
    if ( s_pwr_click )
    {
        s_pwr_click = false;
        m |= KK_BOT;
    }
    return m;
}

bool board_touch(int *x, int *y)
{
    if ( !s_tdown ) { return false; }
    *x = s_tx;
    *y = s_ty;
    return true;
}

static esp_lcd_panel_io_handle_t s_tio;

/* The FT3168 drops into "monitor mode" when nobody touches it and then often NACKs a poll.
 * G_CTRL (0x86) = 0: stay in active mode. */
static void touch_stay_active(void)
{
    uint8_t const v = 0x00;
    if ( s_tio ) { (void)esp_lcd_panel_io_tx_param(s_tio, 0x86, &v, 1); }
}

static esp_err_t touch_start(void)
{
    esp_lcd_touch_config_t const tp_cfg = {
        .x_max = KK_W,
        .y_max = KK_H,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    io_cfg.transaction_timeout_ms = 30;   /* the default waits forever on a stuck bus */
    esp_err_t e = esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &io_cfg, &io);
    if ( ESP_OK == e ) { e = esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &s_tp); }
    if ( ESP_OK == e ) { s_tio = io; touch_stay_active(); }
    return e;
}

static void touch_task(void *arg)
{
    (void)arg;
    int fails = 0;
    uint32_t errs = 0;
    for ( ;; )
    {
        vTaskDelay(pdMS_TO_TICKS(15));
        if ( NULL == s_tp ) { continue; }
        /* read only when the controller says there is something (INT low), or while a finger is down
         * (to see it lift): polling an idle FT3168 gets NACKs every ~14 s */
        if ( !s_tdown && (1 == gpio_get_level(BSP_LCD_TOUCH_INT)) ) { fails = 0; continue; }
        esp_err_t const re = esp_lcd_touch_read_data(s_tp);
        if ( ESP_OK != re )
        {
            ++errs;
            if ( (1 == errs) || (0 == errs % 200) ) { ESP_LOGW(TAG, "touch: read error %s (%lu so far)", esp_err_to_name(re), (unsigned long)errs); }
            /* one miss: keep the last state (a finger does not lift for one bad poll) */
            if ( ++fails == 40 )   /* 0.6 s of nothing: wake the controller up again */
            {
                s_tdown = false;
                ESP_LOGW(TAG, "touch: 40 read errors, resetting touch");
                gpio_set_direction(BSP_LCD_TOUCH_RST, GPIO_MODE_OUTPUT);
                gpio_set_level(BSP_LCD_TOUCH_RST, 0);
                vTaskDelay(pdMS_TO_TICKS(10));
                gpio_set_level(BSP_LCD_TOUCH_RST, 1);
                vTaskDelay(pdMS_TO_TICKS(150));
                touch_stay_active();
                fails = 0;
            }
            continue;
        }
        fails = 0;
        uint16_t tx = 0, ty = 0, str = 0;
        uint8_t n = 0;
        bool const down = esp_lcd_touch_get_coordinates(s_tp, &tx, &ty, &str, &n, 1) && (n > 0);
        if ( down )
        {
            s_tx = tx;
            s_ty = ty;
        }
        s_tdown = down;
    }
}

uint32_t board_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- the Kode Link over USB serial ----
 * In: one line per message ("notify hoi", "sys time <epoch> <offset>", ...); a leading '@' is ignored.
 * Out: "@<message>" lines, so logs and link traffic can share the port. */

static void link_rx_task(void *arg)
{
    (void)arg;
    char line[LINK_LEN];
    int len = 0;
    uint8_t buf[64];
    for ( ;; )
    {
        int const n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for ( int i = 0; i < n; ++i )
        {
            char const c = (char)buf[i];
            if ( ('\n' == c) || ('\r' == c) )
            {
                if ( len > 0 )
                {
                    line[len] = '\0';
                    const char *m = ('@' == line[0]) ? &line[1] : line;
                    if ( !net_cfg_line(m) )
                    {
                        char item[LINK_LEN];
                        (void)snprintf(item, sizeof(item), "%s", m);
                        (void)xQueueSend(s_link_q, item, 0);
                    }
                    len = 0;
                }
            }
            else if ( len < (LINK_LEN - 1) )
            {
                line[len++] = c;
            }
        }
    }
}

static void net_rx(const char *msg, size_t len)
{
    char item[LINK_LEN];
    if ( len >= sizeof(item) ) { len = sizeof(item) - 1; }
    memcpy(item, msg, len);
    item[len] = '\0';
    (void)xQueueSend(s_link_q, item, pdMS_TO_TICKS(20));
}

void kdotsim_link_send(const char *msg, size_t len)
{
    (void)net_send(msg, len);
    if ( !usb_serial_jtag_is_connected() ) { return; }
    char out[LINK_LEN + 2];
    int const n = snprintf(out, sizeof(out), "@%.*s\n", (int)len, msg);
    if ( n > 0 ) { (void)usb_serial_jtag_write_bytes(out, (size_t)n, 0); }
}

int kdotsim_link_recv(char *buf, size_t cap)
{
    char item[LINK_LEN];
    if ( pdTRUE != xQueueReceive(s_link_q, item, 0) ) { return -1; }
    (void)snprintf(buf, cap, "%s", item);
    return (int)strlen(buf);
}

/* ---- bring-up ---- */

void board_init(void)
{
    /* USB serial: console and link. */
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.rx_buffer_size = 1024;
    ucfg.tx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));
    usb_serial_jtag_vfs_use_driver();
    s_link_q = xQueueCreateWithCaps(LINK_DEPTH, LINK_LEN, MALLOC_CAP_SPIRAM);
    if ( NULL == s_link_q ) { s_link_q = xQueueCreate(8, LINK_LEN); }
    xTaskCreate(link_rx_task, "link_rx", 4096, NULL, 4, NULL);

    /* BOOT button. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* I2C devices. */
    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t const bus = bsp_i2c_get_handle();
    s_axp = add_dev(bus, AXP_ADDR, &s_has_axp);
    s_rtc = add_dev(bus, RTC_ADDR, &s_has_rtc);
    if ( s_has_axp )
    {
        (void)reg_set_bits(s_axp, 0x18, 0x08);   /* fuel gauge on */
        (void)reg_set_bits(s_axp, 0x41, 0x08);   /* PWR short-press IRQ on */
        uint8_t const clr[3] = { 0xFF, 0xFF, 0xFF };
        (void)reg_write(s_axp, 0x48, clr, 3);
        xTaskCreate(pmu_task, "pmu", 3072, NULL, 3, NULL);
    }
    if ( s_has_rtc )
    {
        uint8_t ctrl1 = 0;
        if ( reg_read(s_rtc, 0x00, &ctrl1, 1) && (ctrl1 & 0x22U) )
        {
            ctrl1 &= (uint8_t)~0x22U;          /* clock running, 24-hour mode */
            (void)reg_write(s_rtc, 0x00, &ctrl1, 1);
        }
    }

    /* Frame buffers and DMA chunks. */
    for ( int i = 0; i < 2; ++i )
    {
        s_fbs[i] = heap_caps_calloc((size_t)KK_W * KK_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        s_dma[i] = heap_caps_malloc(CHUNK_PX * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_fbs[i] && s_dma[i]);
    }
    s_chunk_done = xSemaphoreCreateCounting(4, 0);
    s_frame_idle = xSemaphoreCreateBinary();
    (void)xSemaphoreGive(s_frame_idle);
    s_frame_q = xQueueCreate(1, sizeof(const uint16_t *));

    /* Panel. */
    bsp_display_config_t const dcfg = { .max_transfer_sz = CHUNK_PX * (int)sizeof(uint16_t) };
    ESP_ERROR_CHECK(bsp_display_new(&dcfg, &s_panel, &s_io));
    esp_lcd_panel_io_callbacks_t const cbs = { .on_color_trans_done = on_chunk_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));
    xTaskCreatePinnedToCore(flush_task, "flush", 4096, NULL, 5, NULL, 1);
    board_brightness(100);
    board_flush(s_fbs[0]);                    /* a black frame, and the light comes on with it */

    /* Touch. */
    esp_log_level_set("FT5x06", ESP_LOG_NONE);              /* a missed read is normal, the task retries */
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    if ( ESP_OK != touch_start() )
    {
        ESP_LOGW(TAG, "touch did not start");
        s_tp = NULL;
    }
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 4, NULL, 1);

    net_init(net_rx);

    ESP_LOGI(TAG, "up: axp=%d rtc=%d touch=%d", s_has_axp, s_has_rtc, NULL != s_tp);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage.c
 * @brief See storage.h.
 */
#include "storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "kk.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static volatile bool s_data_ok, s_sd_ok;

static char s_sd_why[48] = "not tried";
static uint32_t s_sd_mb;
static sdmmc_card_t *s_card;

const char *storage_sd_status(void) { return s_sd_why; }

/* The watch's slot: 1-bit, CLK 2, CMD 1, D0 3, with the internal pull-ups the
 * BSP leaves off, and D3 (GPIO17) held high so the card stays in SD mode. */
static esp_err_t sd_try(int khz)
{
    gpio_config_t const d3 = { .pin_bit_mask = 1ULL << 17, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    (void)gpio_config(&d3);
    gpio_set_level(17, 1);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_2;
    slot.cmd = GPIO_NUM_1;
    slot.d0 = GPIO_NUM_3;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t const mc = { .format_if_mount_failed = false, .max_files = 6, .allocation_unit_size = 16 * 1024 };
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &s_card);
}

static void sd_task(void *arg)
{
    (void)arg;
    /* A card can go in at any time: look again every 10 s until one mounts. */
    for ( ;; )
    {
        esp_err_t e = sd_try(SDMMC_FREQ_DEFAULT);
        if ( (ESP_OK != e) && (ESP_FAIL != e) ) { e = sd_try(SDMMC_FREQ_PROBING); }   /* a slow card, or long wires */
        if ( ESP_OK == e )
        {
            s_sd_ok = true;
            s_sd_mb = (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
            (void)snprintf(s_sd_why, sizeof(s_sd_why), "%s, %lu MB", s_card->cid.name, (unsigned long)s_sd_mb);
            ESP_LOGI(TAG, "microSD at /sdcard: %s", s_sd_why);
            kk_link_send("sd ok %s", s_sd_why);
            break;
        }
        /* ESP_FAIL: the card answered but holds no FAT volume (exFAT, or none) */
        (void)snprintf(s_sd_why, sizeof(s_sd_why), "%s", (ESP_FAIL == e) ? "not FAT32 (exFAT?)" : esp_err_to_name(e));
        static char last[48];
        if ( 0 != strcmp(last, s_sd_why) )
        {
            (void)snprintf(last, sizeof(last), "%s", s_sd_why);
            ESP_LOGW(TAG, "no microSD: %s", s_sd_why);
            kk_link_send("sd fail %s", s_sd_why);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    vTaskDelete(NULL);
}

void storage_init(void)
{
    esp_vfs_fat_mount_config_t const cfg = {
        .format_if_mount_failed = true,     /* the first boot finds it empty */
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    if ( ESP_OK == esp_vfs_fat_spiflash_mount_rw_wl("/data", "storage", &cfg, &s_wl) )
    {
        s_data_ok = true;
    }
    else
    {
        ESP_LOGW(TAG, "no /data");
    }
    xTaskCreate(sd_task, "sd", 4096, NULL, 2, NULL);
}

void storage_info(storage_info_t *si)
{
    uint32_t sz = 0;
    si->flash_mb = (ESP_OK == esp_flash_get_physical_size(NULL, &sz)) ? (sz >> 20) : 0;
#if defined(CONFIG_ESPTOOLPY_FLASHSIZE_32MB) /* the layout: 32 MB since 0.9 */
    si->flash_cfg_mb = 32;
#else
    si->flash_cfg_mb = 16;
#endif
    si->data_ok = s_data_ok;
    si->data_total = si->data_free = 0;
    if ( s_data_ok ) { (void)esp_vfs_fat_info("/data", &si->data_total, &si->data_free); }
    si->sd_ok = s_sd_ok;
    si->sd_total = si->sd_free = 0;
    if ( s_sd_ok ) { (void)esp_vfs_fat_info("/sdcard", &si->sd_total, &si->sd_free); }
}

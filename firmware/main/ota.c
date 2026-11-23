/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ota.c
 * @brief See ota.h.
 */
#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "psa/crypto.h"

#include "kk.h"
#include "ble.h"

static const char *TAG = "ota";
static char s_url[200];
static volatile int s_pct = -1;
static volatile bool s_busy;

static void ota_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t const http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t const cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&cfg, &h);
    if ( ESP_OK == e )
    {
        esp_app_desc_t d;
        if ( (ESP_OK == esp_https_ota_get_img_desc(h, &d)) )
        {
            ESP_LOGI(TAG, "image %s %s", d.project_name, d.version);
        }
        int const total = esp_https_ota_get_image_size(h);
        int last = -1;
        while ( ESP_ERR_HTTPS_OTA_IN_PROGRESS == (e = esp_https_ota_perform(h)) )
        {
            int const got = esp_https_ota_get_image_len_read(h);
            int const pct = (total > 0) ? (int)(((int64_t)got * 100) / total) : 0;
            s_pct = pct;
            if ( (pct / 10) != (last / 10) )
            {
                last = pct;
                kk_link_send("ota %d", pct);
            }
        }
        if ( (ESP_OK == e) && !esp_https_ota_is_complete_data_received(h) )
        {
            e = ESP_FAIL;
        }
        esp_err_t const f = (ESP_OK == e) ? esp_https_ota_finish(h) : (esp_https_ota_abort(h), e);
        e = f;
    }
    if ( ESP_OK == e )
    {
        ESP_LOGI(TAG, "update written; restarting");
        kk_link_send("ota done");
        s_pct = 100;
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    ESP_LOGW(TAG, "update failed: %s", esp_err_to_name(e));
    kk_link_send("ota fail %s", esp_err_to_name(e));
    s_pct = -1;
    s_busy = false;
    vTaskDelete(NULL);
}

static char s_sha[65];

static void kody_task(void *arg)
{
    (void)arg;
    const esp_partition_t *const part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "kody");
    esp_http_client_config_t const http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&http);
    const char *why = NULL;
    static uint8_t buf[4096];
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    (void)psa_crypto_init();
    (void)psa_hash_setup(&sha, PSA_ALG_SHA_256);
    if ( (NULL == part) || (NULL == c) ) { why = "no partition"; }
    else if ( ESP_OK != esp_http_client_open(c, 0) ) { why = "connect"; }
    else
    {
        int64_t const total = esp_http_client_fetch_headers(c);
        if ( (total <= 0) || (total > (int64_t)part->size) ) { why = "size"; }
        else
        {
            size_t done = 0, erased = 0;
            int last = -1;
            while ( (NULL == why) && (done < (size_t)total) )
            {
                int const n = esp_http_client_read(c, (char *)buf, sizeof(buf));
                if ( n <= 0 ) { why = "read"; break; }
                while ( erased < done + (size_t)n )
                {
                    if ( ESP_OK != esp_partition_erase_range(part, erased, 0x10000) ) { why = "erase"; break; }
                    erased += 0x10000;
                }
                if ( why ) { break; }
                if ( ESP_OK != esp_partition_write(part, done, buf, (size_t)n) ) { why = "write"; break; }
                (void)psa_hash_update(&sha, buf, (size_t)n);
                done += (size_t)n;
                int const pct = (int)(((int64_t)done * 100) / total);
                s_pct = pct;
                if ( (pct / 10) != (last / 10) ) { last = pct; kk_link_send("kody %d", pct); }
            }
            if ( NULL == why )
            {
                uint8_t d[32];
                char hex[65];
                size_t dl = 0;
                (void)psa_hash_finish(&sha, d, sizeof(d), &dl);
                for ( int i = 0; i < 32; ++i ) { (void)snprintf(&hex[i * 2], 3, "%02x", d[i]); }
                if ( 0 != strcmp(hex, s_sha) ) { why = "checksum"; }
            }
        }
    }
    (void)psa_hash_abort(&sha);
    if ( c ) { (void)esp_http_client_close(c); (void)esp_http_client_cleanup(c); }
    if ( NULL == why )
    {
        ESP_LOGI(TAG, "Kody written; restarting");
        kk_link_send("kody done");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    ESP_LOGW(TAG, "Kody download failed: %s", why);
    kk_link_send("kody fail %s", why);
    s_pct = -1;
    s_busy = false;
    vTaskDelete(NULL);
}

bool ota_kody(const char *url, const char *sha256)
{
    if ( s_busy || (NULL == url) || (NULL == sha256) || (0 != strncmp(url, "https://", 8))
         || (strlen(url) >= sizeof(s_url)) || (64 != strlen(sha256)) )
    {
        return false;
    }
    (void)snprintf(s_url, sizeof(s_url), "%s", url);
    (void)snprintf(s_sha, sizeof(s_sha), "%s", sha256);
    s_busy = true;
    s_pct = 0;
    if ( pdPASS != xTaskCreate(kody_task, "kodyget", 8192, NULL, 4, NULL) )
    {
        s_busy = false;
        s_pct = -1;
        return false;
    }
    return true;
}

bool ota_start(const char *url)
{
    if ( s_busy || (NULL == url) || (0 != strncmp(url, "https://", 8)) || (strlen(url) >= sizeof(s_url)) )
    {
        return false;
    }
    (void)snprintf(s_url, sizeof(s_url), "%s", url);
    ble_stop();   /* TLS needs the internal RAM Bluetooth holds; the update restarts the watch anyway */
    s_busy = true;
    s_pct = 0;
    if ( pdPASS != xTaskCreate(ota_task, "ota", 8192, NULL, 4, NULL) )
    {
        s_busy = false;
        s_pct = -1;
        return false;
    }
    return true;
}

int ota_progress(void) { return s_pct; }

const char *ota_slot(void)
{
    const esp_partition_t *const p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

bool ota_is_pending(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *const p = esp_ota_get_running_partition();
    return p && (ESP_OK == esp_ota_get_state_partition(p, &st)) && (ESP_OTA_IMG_PENDING_VERIFY == st);
}

void ota_mark_good(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *const p = esp_ota_get_running_partition();
    if ( p && (ESP_OK == esp_ota_get_state_partition(p, &st)) && (ESP_OTA_IMG_PENDING_VERIFY == st) )
    {
        ESP_LOGI(TAG, "this image is good: rollback cancelled");
        (void)esp_ota_mark_app_valid_cancel_rollback();
    }
}

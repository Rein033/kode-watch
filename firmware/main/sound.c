/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sound.c
 * @brief See sound.h. 16 kHz mono sine tones with a short fade in and out (no
 *        clicks); the codec is opened per sound and closed after, so the
 *        amplifier is off between them.
 */
#include "sound.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"

#define RATE 16000
#define CHUNK 256

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_q;
static volatile bool s_on = true;

typedef struct { uint16_t hz, ms; } note_t;

static const note_t NOTE[] = { { 1319, 90 }, { 0, 30 }, { 1760, 140 } };              /* E6, A6 */
static const note_t ALARM[] = { { 2093, 120 }, { 0, 80 }, { 2093, 120 }, { 0, 80 }, { 2093, 120 } };
static const note_t CLICK[] = { { 3000, 6 } };
static const note_t HELLO[] = { { 1047, 80 }, { 1319, 80 }, { 1568, 120 } };

static void tone(uint16_t hz, uint16_t ms)
{
    static int16_t buf[CHUNK];
    uint32_t const total = (uint32_t)RATE * ms / 1000U, fade = (total < 160U) ? total / 4U : 40U;
    float ph = 0.0f;
    float const step = 2.0f * (float)M_PI * (float)hz / (float)RATE;
    for ( uint32_t i = 0; i < total; )
    {
        uint32_t n = 0;
        for ( ; (n < CHUNK) && (i < total); ++n, ++i )
        {
            float env = 1.0f;
            if ( i < fade ) { env = (float)i / (float)fade; }
            else if ( i > total - fade ) { env = (float)(total - i) / (float)fade; }
            buf[n] = (0 == hz) ? 0 : (int16_t)(sinf(ph) * 11000.0f * env);
            ph += step;
            if ( ph > 2.0f * (float)M_PI ) { ph -= 2.0f * (float)M_PI; }
        }
        (void)esp_codec_dev_write(s_spk, buf, (int)(n * sizeof(int16_t)));
    }
}

static void sound_task(void *arg)
{
    (void)arg;
    sound_t s;
    for ( ;; )
    {
        if ( pdTRUE != xQueueReceive(s_q, &s, portMAX_DELAY) ) { continue; }
        const note_t *seq = NOTE;
        size_t n = sizeof(NOTE) / sizeof(NOTE[0]);
        if ( SND_ALARM == s ) { seq = ALARM; n = sizeof(ALARM) / sizeof(ALARM[0]); }
        else if ( SND_CLICK == s ) { seq = CLICK; n = 1; }
        else if ( SND_HELLO == s ) { seq = HELLO; n = sizeof(HELLO) / sizeof(HELLO[0]); }
        esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 1, .bits_per_sample = 16 };
        if ( ESP_CODEC_DEV_OK != esp_codec_dev_open(s_spk, &fs) ) { continue; }
        (void)esp_codec_dev_set_out_vol(s_spk, (SND_CLICK == s) ? 45 : 75);
        for ( size_t i = 0; i < n; ++i ) { tone(seq[i].hz, seq[i].ms); }
        tone(0, 20);   /* let the last samples out before the amplifier goes off */
        (void)esp_codec_dev_close(s_spk);
    }
}

void sound_init(void)
{
    s_spk = bsp_audio_codec_speaker_init();
    if ( NULL == s_spk )
    {
        ESP_LOGW("sound", "no speaker");
        return;
    }
    s_q = xQueueCreate(4, sizeof(sound_t));
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 3, NULL, 0);
    ESP_LOGI("sound", "speaker up");
}

void sound_play(sound_t s)
{
    if ( !s_on || (NULL == s_q) ) { return; }
    (void)xQueueSend(s_q, &s, 0);
}

void sound_set_on(bool on) { s_on = on; }
bool sound_on(void) { return s_on; }

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage.h
 * @brief The watch's storage: the flash chip, the "storage" FAT partition at
 *        /data (the flash the firmware and Kody leave free) and the microSD at /sdcard.
 */
#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t flash_mb;        /* the chip's real size (JEDEC), 0 when unknown */
    uint32_t flash_cfg_mb;    /* what the firmware was built for */
    bool data_ok;
    uint64_t data_total, data_free;
    bool sd_ok;
    uint64_t sd_total, sd_free;
} storage_info_t;

/** @brief Mount /data now and look for a microSD card in the background. */
void storage_init(void);
void storage_info(storage_info_t *si);

/** @brief What the microSD did: its name and size, or why it is not there. */
const char *storage_sd_status(void);

#endif /* STORAGE_H_ */

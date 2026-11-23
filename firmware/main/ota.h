/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ota.h
 * @brief Updates over Wi-Fi: the hub sends "ota <https url>", the watch writes
 *        the image to its other OTA slot, checks it and restarts into it. A
 *        new image that does not stay up (crash before ota_mark_good()) is
 *        rolled back by the bootloader.
 */
#ifndef OTA_H_
#define OTA_H_

#include <stdbool.h>

/** @brief Start an update from @p url (https only); false when one is running or the url is refused. */
bool ota_start(const char *url);

/** @brief Fetch Kody's frames from @p url into the kody partition, checked
 *         against @p sha256 (64 hex), then restart. false when busy or refused. */
bool ota_kody(const char *url, const char *sha256);

/** @brief Percent done while an update runs, -1 otherwise. */
int ota_progress(void);

/** @brief The label of the slot this image runs from ("factory", "ota_0", "ota_1"). */
const char *ota_slot(void);

/** @brief This image works: cancel the rollback (call once the OS has run a while). */
void ota_mark_good(void);
/** @brief true while a fresh update still has to prove itself (it is rolled back on a restart). */
bool ota_is_pending(void);

#endif /* OTA_H_ */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file apps.h
 * @brief The App Store's side of the OS: kode-sdk apps in the `apps` region, in
 *        kodeOS's format (kapps_fmt.h), started through kboot's boot request.
 *
 * The catalog is the hub's index.txt, one app a line: id|name|size|sha256|color|description|flags
 * (flags: "pad" = the app needs the Dot's pad: the SDK's virtual pad is turned on for it).
 * An installed app is a directory entry; its id and name are kept in /data/apps.txt
 * by the entry's seq. Open writes a boot request (NEW) and restarts: kboot boots
 * the app once, and any reset after it - the app's own exit too - comes back here.
 */
#ifndef APPS_H_
#define APPS_H_

#include <stdbool.h>
#include <stdint.h>

#define APPS_MAX 16

typedef struct {
    char id[24];
    char name[24];
    char sha[65];
    uint32_t size;
    uint16_t color;      /* RGB565 */
    char desc[64];
    bool installed;
    bool pad;
} apps_item_t;

/** @brief Fetch the catalog from @p url in the background (https). */
void apps_refresh(const char *url);
/** @brief true while the catalog loads; the list is up to date when false. */
bool apps_loading(void);
/** @brief The catalog joined with what is installed; returns how many. */
int apps_list(apps_item_t *out, int max);
/** @brief Download and install catalog item @p i (from apps_list()) in the background. */
bool apps_install(int i, const char *base_url);
/** @brief Percent while an install runs, -1 otherwise. */
int apps_progress(void);
/** @brief The last problem, "" when there is none. */
const char *apps_error(void);
/** @brief Boot the installed app @p id (restarts; returns false when it cannot). */
bool apps_open(const char *id);
/** @brief Remove the installed app @p id. */
bool apps_remove(const char *id);

#endif /* APPS_H_ */

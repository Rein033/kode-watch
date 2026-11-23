/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FS_PATH 200

typedef struct {
    char name[64];
    uint32_t size;
    bool dir;
} fs_ent_t;

void fs_init(void);
void fs_tick(void);                   /* the OS loop: answers and timeouts */
bool fs_on_line(const char *m);       /* the hub's "fs ..." lines; true when it was one */
bool fs_path_ok(const char *p);
int fs_list(const char *dir, fs_ent_t *out, int max);   /* folders first; -1 on error */
bool fs_remove(const char *path);

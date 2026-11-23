/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
void crash_init(void);
const char *crash_report(void);   /* "boot <reason>" or "crash <reason> task=.. pc=.. bt .." */
void crash_sent(void);            /* the hub has it: the core dump may go */

// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hsm (hardware status monitor) - live monitor for CPU, GPU, memory,
 * power, disks, network and processes on Linux
 * Copyright (C) 2026  Yormask
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* watch.c - the background duties, run after every sample by the screen and
 * by the headless modes alike: session heartbeat (crash detection), flight
 * recorder, alerts, events.log and the daily history, and deleting logs past
 * their age once an hour. It also starts the freeze watchdog. */
#define _GNU_SOURCE
#include "history.h"
#include "log.h"
#include "watch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define KEEP_EVERY 3600.0 /* seconds between clean-ups of old logs */
#define KEEP_DELAY 120.0  /* after the setting changed: time to change one's mind */

notice_t g_notice;

static struct {
    int tui, started;
    double next_keep;
} W;

void notice_add(const char *fmt, ...)
{
    if (g_notice.n >= NOTICE_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_notice.line[g_notice.n++], sizeof g_notice.line[0], fmt, ap);
    va_end(ap);
}

static void housekeeping(ui_state_t *u)
{
    int n = log_housekeeping(config_log_dir(u), u->log_keep_days);
    if (n > 0)
        log_note(0, "Deleted %d log%s older than %d days", n, n == 1 ? "" : "s", u->log_keep_days);
}

void watch_start(sys_t *s, ui_state_t *u, const char *mode, int tui)
{
    W.tui = tui;
    freeze_start();
    netq_set(u->net_check, u->net_check_host);
    netq_read(&s->net.q); /* on now: the logs' columns include it */
    str_copy(g_log.dir, sizeof g_log.dir, config_log_dir(u));
    if (!g_log.nchan)
        log_build_channels(s, u->log_skip);
    if (tui) { /* the screen shows what the check finds; no need to alert about it too */
        session_start(s, mode);
        alert_init(s);
    } else { /* a background run: a crash found at start is worth an alert */
        alert_init(s);
        session_start(s, mode);
    }
    housekeeping(u);
    history_start(s); /* after the session check: its findings go into events.log too */
    W.next_keep = now_sec() + KEEP_EVERY;
    W.started = 1;
}

void watch_tick(sys_t *s, ui_state_t *u)
{
    if (!W.started)
        return;
    if (!g_log.f && !g_log.nplog) /* follows the setting, unless a log is being written */
        str_copy(g_log.dir, sizeof g_log.dir, config_log_dir(u));
    session_tick();
    rec_tick(s, u);
    alert_tick(s, u, W.tui);
    history_tick(s);
    if (now_sec() >= W.next_keep) {
        W.next_keep = now_sec() + KEEP_EVERY;
        housekeeping(u);
    }
}

void watch_keep_changed(void)
{
    W.next_keep = now_sec() + KEEP_DELAY;
}

void watch_end(sys_t *s)
{
    if (!W.started)
        return;
    rec_end(1);
    history_end(s);
    session_end();
    netq_set(0, NULL);
    W.started = 0;
}

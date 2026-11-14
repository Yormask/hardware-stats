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
/* watch.h - what hsm does in the background, with or without the screen:
 * crash detection across reboots, the flight recorder, alerts and
 * deleting old logs */
#ifndef HSM_WATCH_H
#define HSM_WATCH_H

#include "ui.h"

/* ---------------------------------------------------------------- watch.c */

/* mode: "screen", "log" or "watch"; tui: the screen is up (messages go there) */
void watch_start(sys_t *s, ui_state_t *u, const char *mode, int tui);
void watch_tick(sys_t *s, ui_state_t *u); /* after every sample */
void watch_end(sys_t *s);                 /* a normal exit */
void watch_keep_changed(void);            /* "delete logs after" changed: clean up soon */

/* what the start-up check found out about the previous session */
#define NOTICE_LINES 10
typedef struct {
    int n;
    char title[80];
    char line[NOTICE_LINES][160];
    char log[PATHBUF]; /* the lead-up, saved from the flight recorder ("" = none) */
} notice_t;
extern notice_t g_notice;
void notice_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* -------------------------------------------------------------- session.c */

void session_start(sys_t *s, const char *mode); /* checks how the previous sessions ended */
void session_tick(void);                        /* heartbeat */
void session_end(void);
const char *session_dir(void);                  /* ~/.local/state/hsm/sessions */
/* the last unexpected ends, newest first, from the history file */
int session_history(char lines[][160], int max);

/* ------------------------------------------------------------- recorder.c */

typedef struct {
    int on, minutes;    /* as configured */
    int rows, nchan;    /* in memory now */
    int busy;           /* an incident log is being written */
    int saved;          /* incident logs saved since start */
    char last[PATHBUF]; /* the newest one */
    char reason[160];   /* what started it */
} rec_status_t;

void rec_tick(sys_t *s, const ui_state_t *u);
void rec_end(int clean); /* clean: also delete this session's rolling files */
void rec_status(rec_status_t *st);
/* turn the rolling files a session left behind into an incident log */
int rec_recover(long pid, double last_seen, const char *why, char *out, size_t n);

/* ---------------------------------------------------------------- alert.c */

void alert_init(sys_t *s);
void alert_tick(sys_t *s, ui_state_t *u, int tui);
void alert_test(ui_state_t *u, int tui); /* "Send a test alert" */

#endif

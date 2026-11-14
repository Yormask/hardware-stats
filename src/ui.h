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
/* ui.h - terminal interface, settings and plain-text report */
#ifndef HSM_UI_H
#define HSM_UI_H

#include "collect.h"
#include <stdio.h>

enum { VIEW_DASH, VIEW_PROCS, VIEW_HW, VIEW_SETUP, VIEW_LOG, VIEW_SVC, VIEW_EVENTS, VIEW_HIST, VIEW_COUNT };
enum { SORT_CPU, SORT_MEM, SORT_IO, SORT_PID, SORT_NAME, SORT_TIME, SORT_GPU, SORT_FILES, SORT_COUNT };

/* dashboard panels */
enum { P_CPU, P_GPU, P_MEM, P_POWER, P_DISK, P_NET, P_TEMPS, P_FANS, P_FS, P_HEALTH, P_SVC, P_VOLT, P_COUNT };
/* appearance choices */
enum { BAR_BLOCKS, BAR_SQUARES, BAR_LINE, BAR_CLASSIC, BAR_COUNT };
enum { GRAPH_BLOCKS, GRAPH_BRAILLE, GRAPH_OFF, GRAPH_COUNT };
enum { COLOR_VALUE, COLOR_GRADIENT, COLOR_SINGLE, COLOR_MONO, COLOR_COUNT };
/* what the per-core CPU bars show; the other two values are printed next to them */
enum { CORE_USAGE, CORE_TEMP, CORE_CLOCK, CORE_COUNT };
extern const char *const CORE_NAME[CORE_COUNT];


extern const char *const PANEL_KEY[P_COUNT];  /* names used in the config file */
extern const char *const PANEL_NAME[P_COUNT]; /* names shown on screen */
extern const char *const BAR_NAME[BAR_COUNT];
extern const char *const GRAPH_NAME[GRAPH_COUNT];
extern const char *const COLOR_NAME[COLOR_COUNT];
/* colours offered for "Single colour" mode: the terminal's standard ones */
#define SINGLE_COUNT 14
extern const char *const SINGLE_NAME[SINGLE_COUNT];
extern const short SINGLE_COLOR[SINGLE_COUNT]; /* ncurses colour number */
/* the selection highlight uses the same list; the background has its own */
#define BG_COUNT 11
extern const char *const BG_NAME[BG_COUNT];
extern const short BG_COLOR[BG_COUNT]; /* -1 = the terminal's own background */
/* text colours (selected text, titles); entry 0 = automatic */
#define TXT_COUNT 17
extern const char *const TXT_NAME[TXT_COUNT];
extern const short TXT_COLOR[TXT_COUNT];

typedef struct {
    int col[2][P_COUNT], ncol[2]; /* panel order in the left [0] / right [1] column */
    int on[P_COUNT];              /* shown on the dashboard */
    int show_procs;               /* process list below the panels */
    int left_pct;                 /* left column width in %, 0 = automatic */
    int bar_style, graph_style, color_mode;
    int core_bar;     /* CORE_*: what the per-core bars show */
    int core_values;  /* print the other two values (clock, temperature...) next to each bar */
    int mouse;        /* clicks and the wheel */
    int single_color; /* index into SINGLE_NAME / SINGLE_COLOR */
    int highlight;    /* selection bar, active tab, key hints: index into SINGLE_* */
    int background;   /* index into BG_NAME / BG_COLOR, 0 = terminal default */
    int sel_text;     /* text on the highlight: index into TXT_*, 0 = automatic */
    int title_color;  /* box titles and section headings: index into TXT_*, 0 = cyan */
} layout_t;

typedef struct {
    int view, sort, reverse, show_kthreads, paused, help, follow, tree;
    double interval;
    char filter[64];
    int filter_edit;

    /* process selection (kept up to date by the process table) */
    pid_t sel_pid;
    unsigned long long sel_start;
    int sel_kthread;
    char sel_desc[160], sel_user[32];
    int sel_idx, sel_delta, scroll, page_rows;

    int hw_scroll, hw_max;
    int pd_open, pd_view, pd_scroll; /* process details page (Enter), in the view it was opened from */

    /* events (view 7), history (view 8) */
    int ev_level; /* lowest level shown: EV_INFO, EV_WARN, EV_ERR */
    int ev_sel, ev_scroll;
    char ev_filter[64];
    int hist_scroll, hist_sel; /* History view: the first day shown, the selected one (0 = newest) */

    /* Hardware view pages: drive self-tests (D), cooling check (C) */
    int drv_open, drv_sel, drv_confirm; /* confirm: 1 short / 2 extended waits for y */
    int cool_open;

    /* services (view 6) */
    int svc_sort, svc_rev, svc_sel, svc_scroll, svc_rows;
    char svc_filter[64];
    char svc_path[256], svc_name[80]; /* process list limited to this service, "" = all */

    layout_t lay;
    int setup_sel, setup_scroll;
    int sens_open, sens_sel, sens_scroll; /* Setup > Sensor names and limits */
    int sens_edit;                        /* typing: 1 a name, 2 a limit */
    char sens_buf[64], sens_prompt[96];
    int confirm; /* reset waiting for a second Enter, -1 = none */

    /* logging (view 5) */
    char log_dir[PATHBUF]; /* "" = default folder */
    double log_every;      /* seconds between log rows, 0 = every refresh */
    char log_skip[8192];   /* channels switched off, comma separated ids */
    int log_sel, log_scroll;
    double confirm_until;
    int log_keep_days;     /* delete logs older than this, 0 = keep them all */
    int rec_on, rec_minutes; /* flight recorder */

    /* alerts */
    int alert_notify;      /* ALERT_* */
    int alert_cpu_temp;    /* °C, 0 = off */
    int alert_gpu_temp;    /* °C, 0 = off */
    int alert_disk_ms;     /* time per request, 0 = off */
    int alert_mem_pct;     /* memory available below, 0 = off */
    int alert_disk_full;   /* a file system this % full, 0 = off */
    srule_t srule[MAX_SRULES]; /* your sensor names, limits, hidden sensors */
    int nsrule;
    int alert_throttle, alert_problems;
    int alert_gpu_slow;    /* the GPU slowing itself down from heat for minutes */
    int net_check;         /* ping the router and an internet address (netcheck.c) */
    char net_check_host[128]; /* that address; "" = 1.1.1.1 */
    char alert_command[PATHBUF];

    /* log viewer (inside view 5) */
    int log_mode;          /* 0 settings, 1 list of saved logs, 2 viewer */
    int lv_sel, lv_scroll;
    char view_path[PATHBUF];

    /* renice dialog */
    int nice_open, nice_val, nice_old;
    pid_t nice_pid;
    unsigned long long nice_start;
    char nice_desc[160];

    /* signal dialog */
    int kill_open, kill_sel, kill_kthread;
    pid_t kill_pid;
    unsigned long long kill_start;
    char kill_desc[160], kill_user[32];

    /* one-line status message in the footer */
    char msg[320];
    double msg_until;
    int msg_err;
    int bell;        /* ring the terminal bell at the next redraw (alerts) */
    int notice_open; /* the start-up notice about the previous session */
} ui_state_t;

enum { ALERT_OFF, ALERT_DESKTOP, ALERT_BELL, ALERT_BOTH, ALERT_COUNT };

int ui_init(int utf8); /* -1 when the terminal type is unknown, -2 when it can't move the cursor ("dumb") */
void ui_mouse(int on); /* clicks and the wheel on or off */
void ui_end(void);
void ui_draw(sys_t *s, ui_state_t *u);
int ui_key(sys_t *s, ui_state_t *u, int ch); /* returns 0 when the user wants to quit */
void ui_message(ui_state_t *u, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* config.c */
void layout_defaults(layout_t *l);
void config_defaults(ui_state_t *u);
void config_reset_all(ui_state_t *u); /* everything back to defaults (sensor names stay) */
void config_set_srule(ui_state_t *u, const srule_t *r); /* add / change / remove when empty */
const srule_t *config_srule(const ui_state_t *u, const char *id);
int config_load(ui_state_t *u); /* 0 = loaded, -1 = no file (defaults kept) */
int config_save(const ui_state_t *u);
int config_changed(const ui_state_t *u); /* differs from what was loaded / last saved */
const char *config_path(void);
const char *config_log_dir(const ui_state_t *u); /* folder logs are written to */
int config_as_user(void);         /* act as the real (sudo) user: 1 switched, 0 no need, -1 failed */
void config_as_root(int changed); /* undo config_as_user */
int config_mkdir_p(const char *dir);
const char *config_state_dir(void); /* ~/.local/state/hsm (of the real user under sudo) */
int config_user_ids(uid_t *uid, gid_t *gid); /* the real user under sudo: 1, else 0 */
void config_pretty_path(const char *path, char *out, size_t n); /* home -> ~ */

void report_print(sys_t *s, int nproc);
void json_print(sys_t *s, int nproc); /* nproc < 0: every process */
void json_write(FILE *f, sys_t *s, int nproc);
int lv_open(ui_state_t *u, const char *path); /* the log viewer, with this file */

#endif

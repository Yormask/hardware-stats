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
/* ui_int.h - internal interface shared by the ui_*.c files */
#ifndef HSM_UI_INT_H
#define HSM_UI_INT_H

#include "ui.h"
#include <ncurses.h>

/* ------------------------------------------------------------ globals */
extern int U;                 /* UTF-8 drawing characters available */
extern const char *SEP;       /* " · " */
extern const char *DEG;       /* "°C" */
extern const layout_t *g_lay; /* active appearance + layout settings */

extern const char *const SORT_NAMES[];
extern const char *const VIEW_NAMES[];

/* colour pairs (the terminal's standard colours); use CP(), which also
 * handles monochrome mode. C_INFO = labels, C_IN / C_OUT = read+download /
 * write+upload, C_B* = bars, C_G* = graphs */
enum {
    C_TITLE = 1, C_BORDER, C_OK, C_WARN, C_CRIT, C_GRAY, C_HDR, C_THEAD, C_SEL, C_ACC, C_INFO, C_KEY,
    C_IN, C_OUT, C_BLO, C_BMID, C_BHI, C_TRACK, C_GLO, C_GMID, C_GHI, C_SINGLE, C_BASE
};
attr_t cp(int pair);
attr_t gray_attr(void);
void colors_sync(const layout_t *l); /* apply the chosen "Single colour" */
int gradient_on(void);
attr_t bar_attr(double pct);    /* bar colour for a 0-100 value  */
attr_t bar_temp_attr(double c); /* bar colour for a temperature   */
attr_t bar_temp_attr_lim(double c, double lim); /* ... with your own limit (0 = none) */
attr_t temp_attr_lim(double t, double lim);
attr_t graph_attr(double pct);  /* graph colour for a 0-100 height */
#define CP(n) cp(n)
#define A_GRAY gray_attr()

/* --------------------------------------------------------- primitives */
int put(int y, int x, int maxw, attr_t a, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void app(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void fill(int y, int x, int w, attr_t a);
int wrap_text(int y, int x, int w, int maxl, attr_t a, const char *t); /* word-wrapped; lines used */
attr_t pct_attr(double p);
attr_t temp_attr(double t);
void box_(int y, int x, int h, int w, const char *title, const char *info);
void bar(int y, int x, int w, double pct, attr_t a);
void meter(int y, int x, int w, const char *label, int lw, double pct, const char *val, int vw, attr_t a);
int graph_span(int w); /* samples visible in a graph w cells wide */
void graph(int y, int x, int h, int w, const hist_t *hs, double maxv, attr_t fixed, int flip);
void kv(int y, int *x, int xmax, attr_t va, const char *key, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
void dual_graph(int y, int x, int h, int w, const hist_t *up, const hist_t *dn, attr_t ua, attr_t da, const char *ul,
                const char *dl);
/* one value per column (NAN = none); column hl drawn with ha (a cursor) */
void graph_cols(int y, int x, int h, int w, const float *v, double lo, double hi, attr_t a, int hl, attr_t ha);

/* -------------------------------------------------------------- mouse */
enum {
    HIT_KEY = 1,   /* a = the key a click stands for (footer hints, buttons) */
    HIT_VIEW,      /* a = view (the tabs) */
    HIT_PROC,      /* a = row in the process list */
    HIT_PROC_SORT, /* a = SORT_* */
    HIT_SVC,       /* a = row in the services list */
    HIT_SVC_SORT,  /* a = column */
    HIT_SETUP,     /* a = item; b = -1 / +1 for the arrows of a value */
    HIT_LOGCH,     /* a = row of the Logging view (channel, then process logs) */
    HIT_FILE,      /* a = row of the saved logs */
    HIT_COLUMN,    /* log viewer: a = column */
    HIT_GRAPH,     /* log viewer: a = first column of the graph, b = its width */
    HIT_SIGNAL,    /* signal dialog: a = row */
    HIT_NICE,      /* renice dialog: b = -1 / +1 */
    HIT_SCROLL,    /* only scrolls: a = view-specific */
    HIT_SENSOR,    /* Setup > Sensors: a = row */
    HIT_PD,        /* process details: a = PID to open */
    HIT_EVENT,     /* Events view: a = event in the list */
    HIT_DRIVE,     /* drive tests page: a = drive */
    HIT_HIST,      /* History view: a = day in the list (0 = newest) */
};
typedef struct {
    int y, x, h, w, kind, a, b;
} hit_t;
int key_hint(int y, int x, int maxx, const char *key, const char *label, int code);
void hit_reset(void);
void hit_add(int y, int x, int h, int w, int kind, int a, int b);
const hit_t *hit_at(int y, int x);

/* -------------------------------------------------------------- views */
void dashboard(sys_t *s, ui_state_t *u);
void dashboard_preview(const layout_t *l, int y, int x, int h, int w, int hl);
int dashboard_preview_rows(const layout_t *l);
void draw_procs(sys_t *s, ui_state_t *u, int y, int x, int h, int w, int full);
void hardware(sys_t *s, ui_state_t *u);
void setup_view(sys_t *s, ui_state_t *u);
int setup_key(ui_state_t *u, int ch); /* 1 when the key was used */
void log_view(sys_t *s, ui_state_t *u);
void services_view(sys_t *s, ui_state_t *u);
int services_key(sys_t *s, ui_state_t *u, int ch);
void services_click(sys_t *s, ui_state_t *u, int row, int dbl);
void services_sort_click(ui_state_t *u, int sort);
void setup_click(ui_state_t *u, int item, int arrow, int dbl);
void sensors_page(sys_t *s, ui_state_t *u); /* Setup > Sensor names and limits */
int sensors_page_key(ui_state_t *u, int ch);
void sensors_page_click(ui_state_t *u, int row, int dbl);
void pdetail_open(ui_state_t *u, pid_t pid, unsigned long long start); /* Enter on a process */
void pdetail_view(sys_t *s, ui_state_t *u);
int pdetail_key(ui_state_t *u, int ch);
void pdetail_click(sys_t *s, ui_state_t *u, int pid);
void events_view(sys_t *s, ui_state_t *u); /* view 7 */
int events_key(ui_state_t *u, int ch);
void events_click(ui_state_t *u, int k);
void events_release(void); /* the view was left: free what it read */
void cool_view(sys_t *s, ui_state_t *u); /* Hardware view > C */
int cool_key(sys_t *s, ui_state_t *u, int ch);
int cool_guard(sys_t *s, ui_state_t *u, int k); /* 1: a running check holds the view (asks first) */
void drives_open(sys_t *s, ui_state_t *u); /* Hardware view > D */
void drives_view(sys_t *s, ui_state_t *u);
int drives_key(ui_state_t *u, int ch);
void drives_click(ui_state_t *u, int i);
void history_view(sys_t *s, ui_state_t *u); /* view 8 */
void history_click(ui_state_t *u, int k);
int history_key(ui_state_t *u, int ch);
void log_click(sys_t *s, ui_state_t *u, int row, int box, int dbl);
int svc_panel_need(sys_t *s, ui_state_t *u, int w);
void svc_panel(sys_t *s, ui_state_t *u, int y, int x, int h, int w);
void event_line(int y, int x, int w, const event_t *e); /* "12:03:44  disk  nvme0n1: I/O error" */
int core_bar_metric(const sys_t *s, int want); /* CORE_* the core bars can show on this CPU */
extern const char *const NOTIFY_NAME[ALERT_COUNT];
extern const int KEEP_DAYS[];
extern const int NKEEP;
void keep_text(int days, char *out, size_t n);  /* "30 days", "Never" */
void keep_changed(ui_state_t *u);               /* after "delete logs after" changed */

int log_view_key(sys_t *s, ui_state_t *u, int ch);
/* saved logs: the list (log_mode 1) and the viewer (log_mode 2) */
void lv_list_draw(sys_t *s, ui_state_t *u);
int lv_list_key(sys_t *s, ui_state_t *u, int ch);
void lv_list_click(ui_state_t *u, int row, int dbl);
void lv_draw(sys_t *s, ui_state_t *u);
int lv_key(sys_t *s, ui_state_t *u, int ch);
void lv_click(ui_state_t *u, int kind, int a, int b, int x, int dbl);
void lv_wheel(int where, int dir);
void lv_digest(const char *path, char *out, size_t n); /* the gist of a log, one line */
void log_toggle_system(sys_t *s, ui_state_t *u);  /* key l */
void log_toggle_process(sys_t *s, ui_state_t *u); /* key L */

#endif

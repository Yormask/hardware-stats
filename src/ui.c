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
/* ui.c - header, footer, help, the signal dialog, key handling and view dispatch */
#define _GNU_SOURCE
#include "cooling.h"
#include "log.h"
#include "ui_int.h"
#include "watch.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void ui_message(ui_state_t *u, int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->msg, sizeof u->msg, fmt, ap);
    va_end(ap);
    u->msg_err = err;
    u->msg_until = now_sec() + 5.0;
}

/* ======================================================== header/footer */

static void header(sys_t *s, ui_state_t *u)
{
    int x = 0;
    x += put(0, x, COLS - x, CP(C_HDR) | A_BOLD, " %s ", APP_NAME);
    x += 1;
    static const char *const SHORT[VIEW_COUNT] = { "Dash", "Procs", "HW", "Setup", "Log", "Svc", "Events", "Hist" };
    /* full names, short ones, or only the numbers with the open view's name:
     * whatever leaves room for the clock and an alert on the right */
    int room = COLS - x - 24, style = 0;
    for (; style < 2; style++) {
        int need = 0;
        for (int v = 0; v < VIEW_COUNT; v++)
            need += 4 + (int)strlen(style ? SHORT[v] : VIEW_NAMES[v]);
        if (need <= room)
            break;
    }
    for (int v = 0; v < VIEW_COUNT; v++) {
        attr_t a = v == u->view ? CP(C_HDR) | A_BOLD : A_GRAY;
        const char *nm = style == 0 ? VIEW_NAMES[v] : style == 1 || v == u->view ? SHORT[v] : NULL;
        int w = nm ? put(0, x, COLS - x, a, " %d %s ", v + 1, nm) : put(0, x, COLS - x, a, " %d ", v + 1);
        hit_add(0, x, 1, w, HIT_VIEW, v, 0);
        x += w;
    }
    x += 2;
    char rec[40] = "", clock[16], up[24];
    if (g_log.f || g_log.nplog) { /* recording indicator, kept next to the clock */
        char r[24] = "";
        if (g_log.f && g_log.nplog)
            sfmt(r, sizeof r, " +%d", g_log.nplog);
        else if (!g_log.f)
            sfmt(r, sizeof r, " %d proc", g_log.nplog);
        sfmt(rec, sizeof rec, " %s REC%s ", U ? "●" : "*", r);
    }
    int rw = utf8_cols(rec);
    /* something wrong right now (throttling, out of memory...): say so in every view */
    char al[48] = "", what[40];
    int alvl = health_alert(&s->health, what, sizeof what);
    if (alvl)
        sfmt(al, sizeof al, " %s %s ", U ? "⚠" : "!", COLS < 110 ? (alvl > 1 ? "problem" : "warning") : what);
    int aw = utf8_cols(al);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(clock, sizeof clock, "%H:%M:%S", &tm);
    int cw = (int)strlen(clock), xm = COLS - cw - 2 - (rw ? rw + 1 : 0) - (aw ? aw + 1 : 0);
    fmt_duration(s->cpu.uptime, up, sizeof up);
    kv(0, &x, xm, A_BOLD, "", "%s", s->host);
    kv(0, &x, xm, A_NORMAL, "", "%s", s->os);
    kv(0, &x, xm, A_NORMAL, "kernel", "%s", s->kernel);
    kv(0, &x, xm, A_NORMAL, "up", "%s", up);
    if (rw)
        put(0, COLS - cw - 2 - rw, rw, CP(C_CRIT) | A_BOLD | A_REVERSE, "%s", rec);
    if (aw)
        put(0, COLS - cw - 2 - (rw ? rw + 1 : 0) - aw, aw,
            (alvl > 1 ? CP(C_CRIT) : CP(C_WARN)) | A_BOLD | A_REVERSE, "%s", al);
    put(0, COLS - cw - 2, cw + 2, A_BOLD, " %s ", clock);
}

static void footer(ui_state_t *u)
{
    int y = LINES - 1, x = 0;
    if (u->view == VIEW_SETUP && u->sens_open && u->sens_edit) {
        x += put(y, x, COLS, CP(C_KEY), " %s ", u->sens_edit == 1 ? "Name" : "Limit");
        if (u->msg_err && u->msg[0] && now_sec() < u->msg_until) { /* what was wrong with the input */
            put(y, x, COLS - x, CP(C_CRIT) | A_BOLD, " %s", u->msg);
            return;
        }
        x += put(y, x, COLS - x, A_NORMAL, " %s", u->sens_prompt);
        put(y, x, COLS - x, A_GRAY, "   Enter: save   Esc: cancel%s",
            u->sens_edit == 1 ? "   (empty: the chip's name)" : "   (empty: none)");
        return;
    }
    if (u->filter_edit) { /* what is typed goes to the filter of the view */
        const char *f = u->view == VIEW_SVC ? u->svc_filter : u->view == VIEW_EVENTS ? u->ev_filter : u->filter;
        x += put(y, x, COLS, CP(C_KEY), " Filter ");
        x += put(y, x, COLS - x, A_BOLD, " %s", f);
        x += put(y, x, 1, A_REVERSE, " ");
        put(y, x, COLS - x, A_GRAY, "   Enter: keep   Esc: clear");
        return;
    }
    char st[64] = "";
    if (u->paused)
        app(st, sizeof st, "PAUSED ");
    app(st, sizeof st, "%.2gs ", u->interval);
    int sw = (int)strlen(st), xm = COLS - sw - 1;
    put(y, COLS - sw - 1, sw, u->paused ? CP(C_CRIT) | A_BOLD | A_REVERSE : A_GRAY, "%s", st);

    if (u->msg[0] && now_sec() < u->msg_until) {
        put(y, 0, xm, u->msg_err ? CP(C_CRIT) | A_BOLD : CP(C_OK) | A_BOLD, " %s", u->msg);
        return;
    }

    typedef struct {
        const char *k, *l;
        int code; /* the key a click presses, 0 = none */
    } hint_t;
    static const hint_t keys[] = {
        { "q", "Quit", 'q' },     { "?", "Help", '?' },      { "s", "Setup", 's' },        { "l", "Log", 'l' },
        { "Enter", "Details", '\n' },
        { "x", "Kill", 'x' },     { "N", "Nice", 'N' },      { "T", "Tree", 'T' },         { "L", "Log proc", 'L' },
        { "1-8", "View", 0 },     { "c/m/d/g/o/p/n/t", "Sort", 0 }, { "/", "Filter", '/' }, { "b", "CPU bars", 'b' },
        { "k", "Kthreads", 'k' }, { "f", "Follow", 'f' },    { "+/-", "Interval", 0 },     { "Spc", "Pause", ' ' },
    };
    static const hint_t lkeys[] = {
        { "l", "Start/stop", 'l' }, { "Space", "Select", ' ' }, { "v", "Saved logs", 'v' }, { "r", "Recorder", 'r' },
        { "d", "Delete after", 'd' }, { "a", "All", 'a' },      { "n", "None", 'n' },        { "i", "Interval", 'i' },
        { "q", "Quit", 'q' },
    };
    static const hint_t listkeys[] = {
        { "Enter", "Open", '\n' }, { "Up/Dn", "Select", 0 }, { "Esc", "Back", 27 }, { "q", "Quit", 'q' },
    };
    static const hint_t viewkeys[] = {
        { "Left/Right", "Cursor", 0 }, { "Up/Dn", "Column", 0 }, { "+/-", "Zoom", 0 },  { "0", "Fit", '0' },
        { "n/N", "Next/prev event", 0 }, { "[ ]", "Scroll", 0 }, { "Space", "Pin graph", ' ' },
        { "s", "Save", 's' },            { "Esc", "Back", 27 },   { "q", "Quit", 'q' },
    };
    static const hint_t vkeys[] = {
        { "Enter", "Show processes", '\n' }, { "c/m/d/n/t", "Sort", 0 }, { "/", "Filter", '/' }, { "Esc", "Clear", 27 },
        { "1-8", "View", 0 },                 { "?", "Help", '?' },        { "q", "Quit", 'q' },
    };
    static const hint_t evkeys[] = {
        { "Up/Dn", "Select", 0 }, { "/", "Search", '/' }, { "e", "Levels", 'e' }, { "Esc", "Clear", 27 },
        { "1-8", "View", 0 },     { "?", "Help", '?' },   { "q", "Quit", 'q' },
    };
    static const hint_t hikeys[] = {
        { "Up/Dn", "Day", 0 }, { "1-8", "View", 0 }, { "?", "Help", '?' }, { "q", "Quit", 'q' },
    };
    static const hint_t hwkeys[] = {
        { "Up/Dn", "Scroll", 0 }, { "D", "Drive tests", 'D' }, { "C", "Cooling check", 'C' },
        { "1-8", "View", 0 },     { "?", "Help", '?' },        { "q", "Quit", 'q' },
    };
    static const hint_t drvkeys[] = {
        { "Up/Dn", "Drive", 0 }, { "s", "Short test", 's' }, { "e", "Extended test", 'e' }, { "a", "Stop test", 'a' },
        { "r", "Read again", 'r' }, { "Esc", "Back", 27 },   { "q", "Quit", 'q' },
    };
    static const hint_t coolkeys[] = {
        { "Enter", "Start", '\n' }, { "Esc", "Stop / back", 27 }, { "q", "Quit", 'q' },
    };
    static const hint_t pdkeys[] = {
        { "Esc", "Back", 27 }, { "Up/Dn", "Scroll", 0 }, { "x", "Kill", 'x' }, { "N", "Nice", 'N' },
        { "L", "Log it", 'L' }, { "q", "Quit", 'q' },
    };
    static const hint_t senskeys[] = {
        { "Up/Dn", "Select", 0 }, { "Enter", "Rename", '\n' }, { "l", "Limit", 'l' }, { "h", "Hide", 'h' },
        { "H", "Hide unchanging", 'H' }, { "d", "Undo yours", 'd' }, { "Esc", "Back", 27 }, { "q", "Quit", 'q' },
    };
    static const hint_t skeys[] = {
        { "Up/Dn", "Select", 0 }, { "Enter", "Toggle", '\n' }, { "Left/Right", "Column / value", 0 },
        { "[ ]", "Move", 0 },     { "R", "Reset all", 0 },     { "Esc", "Back", 27 },  { "q", "Quit", 'q' },
    };
#define PICK(t) (k = (t), nk = sizeof(t) / sizeof(t)[0])
    const hint_t *k;
    size_t nk;
    if (u->pd_open && u->view == u->pd_view)
        PICK(pdkeys);
    else if (u->view == VIEW_HW && u->drv_open)
        PICK(drvkeys);
    else if (u->view == VIEW_HW && u->cool_open)
        PICK(coolkeys);
    else if (u->view == VIEW_HW)
        PICK(hwkeys);
    else if (u->view == VIEW_SETUP && u->sens_open)
        PICK(senskeys);
    else if (u->view == VIEW_SETUP)
        PICK(skeys);
    else if (u->view == VIEW_LOG)
        u->log_mode == 1 ? PICK(listkeys) : u->log_mode == 2 ? PICK(viewkeys) : PICK(lkeys);
    else if (u->view == VIEW_SVC)
        PICK(vkeys);
    else if (u->view == VIEW_EVENTS)
        PICK(evkeys);
    else if (u->view == VIEW_HIST)
        PICK(hikeys);
    else
        PICK(keys);
#undef PICK
    for (size_t i = 0; i < nk && x < xm; i++) {
        int need = (int)strlen(k[i].k) + (int)strlen(k[i].l) + 3;
        if (x + need > xm)
            break;
        int x0 = x;
        x += put(y, x, xm - x, CP(C_KEY), "%s", k[i].k);
        x += put(y, x, xm - x, A_NORMAL, " %s  ", k[i].l);
        if (k[i].code)
            hit_add(y, x0, 1, x - x0 - 2, HIT_KEY, k[i].code, 0);
    }
}

static void help_overlay(void)
{
    static const char *const L[][2] = {
        { "1 - 6, Tab", "Dashboard Processes Hardware Setup Logging Services" },
        { "7 8", "Events (kept across runs) / History (a day each; what ran)" },
        { "Enter", "everything about the selected process (Esc: back)" },
        { "s, F2", "Setup: choose, arrange and style the panels" },
        { "Up Down PgUp PgDn", "move the selection or scroll (Home / End too)" },
        { "l", "start / stop logging sensors to a CSV file" },
        { "L", "start / stop logging the selected process" },
        { "x, F9", "send a signal to (kill) the selected process" },
        { "N, F7 F8", "priority (nice) of the selected process" },
        { "T, F5", "process tree: which process started which" },
        { "c m d p n t", "sort: CPU mem I/O PID name time (twice: reverse)" },
        { "g o", "sort by GPU use / by open files" },
        { "Enter (6)", "Services: show the processes of the selected one" },
        { "/", "filter by name, command, user or PID (Esc clears)" },
        { "f k", "follow the process, not the row / kernel threads" },
        { "b", "CPU core bars: usage, temperature or clock speed" },
        { "v (5)", "saved logs: a summary, graphs, events; s saves it" },
        { "D C (3)", "Hardware view: drive self-tests, cooling check" },
        { "+ -", "slower / faster refresh" },
        { "Space", "pause / resume sampling" },
        { "Mouse", "click tabs, rows and headings; the wheel scrolls" },
        { "q, F10", "quit" },
    };
    int n = (int)(sizeof L / sizeof L[0]);
    int w = MIN(76, COLS), h = MIN(n + 9, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2);
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Help", APP_NAME " " APP_VERSION "  (any key closes)");
    for (int i = 0; i < n && 2 + i < h - 6; i++) {
        put(y + 2 + i, x + 3, 18, CP(C_INFO) | A_BOLD, "%s", L[i][0]);
        put(y + 2 + i, x + 22, w - 25, A_NORMAL, "%s", L[i][1]);
    }
    put(y + h - 5, x + 3, w - 6, A_GRAY, "Run as root (sudo hsm) for CPU power, exact Intel GPU load, the kernel");
    put(y + h - 4, x + 3, w - 6, A_GRAY, "log, NVMe and SATA drive health and other users' processes.");
    put(y + h - 3, x + 3, w - 6, A_BOLD, "%s (%s)  %s", APP_NAME, APP_LONG_NAME, APP_COPYRIGHT);
    put(y + h - 2, x + 3, w - 6, A_GRAY, "ABSOLUTELY NO WARRANTY. Free software: GPL v3 or later (see LICENSE).");
}

/* ======================================================== signal dialog */

static const struct {
    int sig;
    const char *name, *desc;
} SIGS[] = {
    { SIGTERM, "SIGTERM", "ask it to quit (normal, safe)" },
    { SIGKILL, "SIGKILL", "force kill, can't be ignored" },
    { SIGINT, "SIGINT", "interrupt, like Ctrl+C" },
    { SIGHUP, "SIGHUP", "hang up (many services reload)" },
    { SIGSTOP, "SIGSTOP", "pause / freeze the process" },
    { SIGCONT, "SIGCONT", "resume a paused process" },
};
#define NSIGS ((int)(sizeof SIGS / sizeof SIGS[0]))

static void kill_dialog(ui_state_t *u)
{
    int w = MIN(66, COLS), h = MIN(NSIGS + 9, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2), iw = w - 6;
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Send signal", NULL);
    int xx = x + 3;
    xx += put(y + 2, xx, iw, A_GRAY, "PID ");
    xx += put(y + 2, xx, x + 3 + iw - xx, A_BOLD, "%d", (int)u->kill_pid);
    put(y + 2, xx, x + 3 + iw - xx, A_GRAY, "  user %s", u->kill_user);
    put(y + 3, x + 3, iw, A_NORMAL, "%s", u->kill_desc);
    if (u->kill_kthread)
        put(y + 4, x + 3, iw, CP(C_WARN), "Kernel thread: it ignores most signals.");
    else if (u->kill_pid == getpid())
        put(y + 4, x + 3, iw, CP(C_WARN), "This is %s itself.", APP_NAME);
    else if (u->kill_pid == 1)
        put(y + 4, x + 3, iw, CP(C_WARN), "PID 1 is the init system: the kernel protects it.");
    hit_add(y, x, h, w, HIT_SCROLL, -1, 0); /* clicks inside the dialog stay in it */
    for (int i = 0; i < NSIGS && 5 + i < h - 2; i++) {
        int yy = y + 5 + i, sel = i == u->kill_sel;
        attr_t a = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, x + 2, w - 4, a);
        hit_add(yy, x + 2, 1, w - 4, HIT_SIGNAL, i, 0);
        int cx = x + 3;
        cx += put(yy, cx, iw, a | A_BOLD, "%2d %-8s", SIGS[i].sig, SIGS[i].name);
        put(yy, cx + 1, x + 3 + iw - cx - 1, sel ? a : (i == 1 ? CP(C_CRIT) : A_GRAY), "%s", SIGS[i].desc);
    }
    put(y + h - 2, x + 3, iw, A_GRAY, "Up/Down choose   Enter send   Esc cancel");
}

static void kill_open(ui_state_t *u)
{
    if (u->view != VIEW_PROCS && !(u->view == VIEW_DASH && u->lay.show_procs)) {
        ui_message(u, 1, "Open the process list first (press 2)");
        return;
    }
    if (!u->sel_pid) {
        ui_message(u, 1, "No process selected");
        return;
    }
    u->kill_open = 1;
    u->kill_sel = 0;
    u->kill_pid = u->sel_pid;
    u->kill_start = u->sel_start;
    u->kill_kthread = u->sel_kthread;
    str_copy(u->kill_desc, sizeof u->kill_desc, u->sel_desc);
    str_copy(u->kill_user, sizeof u->kill_user, u->sel_user);
}

static void kill_key(ui_state_t *u, int ch)
{
    switch (ch) {
    case KEY_UP: u->kill_sel = (u->kill_sel + NSIGS - 1) % NSIGS; break;
    case KEY_DOWN: u->kill_sel = (u->kill_sel + 1) % NSIGS; break;
    case '\n':
    case '\r':
    case KEY_ENTER: {
        int i = u->kill_sel;
        int rc = proc_send_signal(u->kill_pid, u->kill_start, SIGS[i].sig);
        char who[40];
        str_copy(who, sizeof who, u->kill_desc);
        if (!rc)
            ui_message(u, 0, "Sent %s to %d (%s)", SIGS[i].name, (int)u->kill_pid, who);
        else if (rc == EPERM)
            ui_message(u, 1, "Permission denied: PID %d belongs to %s. Run hsm with sudo to signal it.",
                       (int)u->kill_pid, u->kill_user);
        else if (rc == ESRCH)
            ui_message(u, 1, "PID %d has already exited", (int)u->kill_pid);
        else
            ui_message(u, 1, "Could not signal PID %d: %s", (int)u->kill_pid, strerror(rc));
        u->kill_open = 0;
        break;
    }
    case 27:
    case 'q':
    case 'x':
    case KEY_F(9): u->kill_open = 0; break;
    }
}

/* ======================================================== renice dialog */

static int proc_view(const ui_state_t *u)
{
    return u->view == VIEW_PROCS || (u->view == VIEW_DASH && u->lay.show_procs);
}

static proc_t *selected_proc(sys_t *s, const ui_state_t *u)
{
    if (!u->sel_pid)
        return NULL;
    for (int i = 0; i < s->procs.n; i++)
        if (s->procs.p[i].pid == u->sel_pid && s->procs.p[i].start == u->sel_start)
            return &s->procs.p[i];
    return NULL;
}

static void nice_apply(ui_state_t *u, pid_t pid, unsigned long long start, int old, int val, const char *desc)
{
    char who[48];
    str_copy(who, sizeof who, desc);
    int rc = proc_renice(pid, start, val);
    if (!rc)
        ui_message(u, 0, "PID %d (%s): nice %d -> %d%s", (int)pid, who, old, val,
                   val < old ? ", higher priority" : val > old ? ", lower priority" : "");
    else if ((rc == EACCES || rc == EPERM) && val < old)
        ui_message(u, 1, "Only root can raise a priority (lower its nice value): run hsm with sudo");
    else if (rc == EACCES || rc == EPERM)
        ui_message(u, 1, "Permission denied: PID %d belongs to another user. Run hsm with sudo.", (int)pid);
    else if (rc == ESRCH)
        ui_message(u, 1, "PID %d has already exited", (int)pid);
    else
        ui_message(u, 1, "Could not change the priority of PID %d: %s", (int)pid, strerror(rc));
}

static proc_t *nice_target(sys_t *s, ui_state_t *u)
{
    if (!proc_view(u)) {
        ui_message(u, 1, "Open the process list first (press 2)");
        return NULL;
    }
    proc_t *p = selected_proc(s, u);
    if (!p)
        ui_message(u, 1, "No process selected");
    else if (p->kthread)
        ui_message(u, 1, "[%s] is a kernel thread: its priority is the kernel's business", p->comm);
    return p && !p->kthread ? p : NULL;
}

static void nice_open(sys_t *s, ui_state_t *u)
{
    proc_t *p = nice_target(s, u);
    if (!p)
        return;
    u->nice_open = 1;
    u->nice_pid = p->pid;
    u->nice_start = p->start;
    u->nice_old = u->nice_val = p->nice;
    str_copy(u->nice_desc, sizeof u->nice_desc, u->sel_desc);
}

/* F7 / F8: one step at a time, no dialog (like htop) */
static void nice_step(sys_t *s, ui_state_t *u, int dir)
{
    proc_t *p = nice_target(s, u);
    if (!p)
        return;
    int v = CLAMP(p->nice + dir, -20, 19);
    if (v != p->nice)
        nice_apply(u, p->pid, p->start, p->nice, v, u->sel_desc);
}

static void nice_dialog(ui_state_t *u)
{
    int w = MIN(66, COLS), h = MIN(13, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2), iw = w - 6;
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Priority (nice)", NULL);
    hit_add(y, x, h, w, HIT_SCROLL, -1, 0);
    int xx = x + 3;
    xx += put(y + 2, xx, iw, A_GRAY, "PID ");
    put(y + 2, xx, x + 3 + iw - xx, A_BOLD, "%d", (int)u->nice_pid);
    put(y + 3, x + 3, iw, A_NORMAL, "%s", u->nice_desc);

    /* -20 ━━━━━━━●━━━━━━━ 19: click anywhere on it */
    int sw = MIN(iw - 8, 40), sx = x + 3 + 4, v = u->nice_val;
    put(y + 5, x + 3, 4, A_GRAY, "-20");
    for (int i = 0; i < sw; i++) {
        int val = -20 + (int)((i + 0.5) * 40 / sw);
        int here = val == v || (i == (v + 20) * sw / 40);
        put(y + 5, sx + i, 1, here ? CP(C_TITLE) | A_BOLD : i == (u->nice_old + 20) * sw / 40 ? A_BOLD : A_GRAY, "%s",
            here ? (U ? "●" : "O") : i == (u->nice_old + 20) * sw / 40 ? "|" : (U ? "━" : "-"));
        hit_add(y + 5, sx + i, 1, 1, HIT_NICE, CLAMP(val, -20, 19), 0);
    }
    put(y + 5, sx + sw + 1, 3, A_GRAY, "19");

    xx = x + 3;
    hit_add(y + 7, xx, 1, 2, HIT_NICE, 0, -1);
    xx += put(y + 7, xx, iw, CP(C_INFO) | A_BOLD, "%s", U ? "◀ " : "< ");
    xx += put(y + 7, xx, x + 3 + iw - xx, A_BOLD, "nice %d", v);
    hit_add(y + 7, xx, 1, 2, HIT_NICE, 0, 1);
    xx += put(y + 7, xx, x + 3 + iw - xx, CP(C_INFO) | A_BOLD, "%s", U ? " ▶" : " >");
    if (v != u->nice_old)
        put(y + 7, xx, x + 3 + iw - xx, A_GRAY, "   now %d", u->nice_old);
    put(y + 8, x + 3, iw, v < 0 ? CP(C_WARN) : A_NORMAL, "%s",
        v < 0   ? "Higher priority: it gets the CPU before the others."
        : v > 0 ? "Lower priority: it gives way when others want the CPU."
                : "Normal priority.");
    if (geteuid() != 0 && v < u->nice_old)
        put(y + 9, x + 3, iw, CP(C_WARN), "Raising a priority (lowering nice) needs root: sudo hsm");
    xx = x + 3;
    xx += key_hint(y + h - 2, xx, x + w - 2, "Left/Right", "change", 0);
    xx += key_hint(y + h - 2, xx, x + w - 2, "Enter", "apply", '\n');
    key_hint(y + h - 2, xx, x + w - 2, "Esc", "cancel", 27);
}

static void nice_key(ui_state_t *u, int ch)
{
    switch (ch) {
    case KEY_LEFT:
    case '-': u->nice_val--; break;
    case KEY_RIGHT:
    case '+':
    case '=': u->nice_val++; break;
    case KEY_HOME: u->nice_val = -20; break;
    case KEY_END: u->nice_val = 19; break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        u->nice_open = 0;
        if (u->nice_val != u->nice_old)
            nice_apply(u, u->nice_pid, u->nice_start, u->nice_old, u->nice_val, u->nice_desc);
        break;
    case 27:
    case 'q':
    case 'N': u->nice_open = 0; break;
    }
    u->nice_val = CLAMP(u->nice_val, -20, 19);
}

/* ============================================= notice about the last session */

static void notice_dialog(void)
{
    int n = g_notice.n;
    int w = MIN(80, COLS), h = MIN(n + 6, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2);
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, g_notice.title, NULL);
    hit_add(y, x, h, w, HIT_KEY, 27, 0);
    for (int i = 0; i < n && 2 + i < h - 2; i++)
        put(y + 2 + i, x + 3, w - 6, i == 0 ? CP(C_CRIT) | A_BOLD : A_NORMAL, "%s", g_notice.line[i]);
    int xx = x + 3;
    if (g_notice.log[0])
        xx += key_hint(y + h - 2, xx, x + w - 2, "v", "graph that log", 'v');
    key_hint(y + h - 2, xx, x + w - 2, "any key", "closes", 0);
}

/* ================================================================== mouse */

static int g_click_y = -1, g_click_x;
static double g_click_t;

static int mouse_event(sys_t *s, ui_state_t *u)
{
    MEVENT ev;
    if (getmouse(&ev) != OK)
        return 1;
    const hit_t *h = hit_at(ev.y, ev.x);
    int dir = ev.bstate & BUTTON4_PRESSED ? -1 : 0;
#ifdef BUTTON5_PRESSED
    if (ev.bstate & BUTTON5_PRESSED)
        dir = 1;
#endif
    if (dir) { /* the wheel scrolls (or zooms the graph in the log viewer) */
        if (u->help || u->notice_open || u->nice_open)
            return 1;
        if (u->kill_open)
            return ui_key(s, u, dir < 0 ? KEY_UP : KEY_DOWN);
        if (u->view == VIEW_LOG && u->log_mode == 2) {
            lv_wheel(h && h->kind == HIT_SCROLL ? h->a : h && h->kind == HIT_COLUMN ? 1 : 0, dir);
            return 1;
        }
        for (int i = 0; i < 3; i++)
            ui_key(s, u, dir < 0 ? KEY_UP : KEY_DOWN);
        return 1;
    }
    if (!(ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)))
        return 1;
    double now = now_sec();
    int dbl = (ev.bstate & BUTTON1_DOUBLE_CLICKED) ||
              (now - g_click_t < 0.45 && ev.y == g_click_y && abs(ev.x - g_click_x) <= 3);
    g_click_t = dbl ? 0 : now; /* a third click starts over */
    g_click_y = ev.y;
    g_click_x = ev.x;
    if (u->help) {
        u->help = 0;
        return 1;
    }
    if (!h)
        return 1;
    if ((u->kill_open || u->nice_open || u->notice_open) && h->kind == HIT_SCROLL)
        return 1;
    switch (h->kind) {
    case HIT_KEY: return ui_key(s, u, h->a);
    case HIT_VIEW:
        if (u->view == VIEW_HW && h->a != VIEW_HW && cool_guard(s, u, -1 - h->a))
            break; /* a cooling check runs: the first click only asks */
        if (h->a == VIEW_LOG && u->view == VIEW_LOG) /* the tab of the view you're in: back to its start */
            u->log_mode = 0;
        u->view = h->a;
        break;
    case HIT_PROC:
        if (dbl && h->a == u->sel_idx && u->sel_pid) /* double click: the details */
            pdetail_open(u, u->sel_pid, u->sel_start);
        else
            u->sel_delta = h->a - u->sel_idx;
        break;
    case HIT_PROC_SORT: return ui_key(s, u, -1000 - h->a);
    case HIT_SVC: services_click(s, u, h->a, dbl); break;
    case HIT_SVC_SORT: services_sort_click(u, h->a); break;
    case HIT_SETUP: setup_click(u, h->a, h->b, dbl); break;
    case HIT_SENSOR: sensors_page_click(u, h->a, dbl); break;
    case HIT_PD: pdetail_click(s, u, h->a); break;
    case HIT_EVENT: events_click(u, h->a); break;
    case HIT_HIST: history_click(u, h->a); break;
    case HIT_DRIVE: drives_click(u, h->a); break;
    case HIT_LOGCH: log_click(s, u, h->a, h->b, dbl); break;
    case HIT_FILE:
        if (u->log_mode == 1)
            lv_list_click(u, h->a, dbl);
        else
            lv_click(u, HIT_FILE, h->a, 0, ev.x, dbl);
        break;
    case HIT_COLUMN:
    case HIT_GRAPH: lv_click(u, h->kind, h->a, h->b, ev.x, dbl); break;
    case HIT_SIGNAL:
        u->kill_sel = h->a;
        if (dbl)
            kill_key(u, '\n');
        break;
    case HIT_NICE:
        u->nice_val = CLAMP(h->b ? u->nice_val + h->b : h->a, -20, 19);
        break;
    }
    return 1;
}

/* ================================================================= main */

void ui_draw(sys_t *s, ui_state_t *u)
{
    g_lay = &u->lay;
    colors_sync(&u->lay);
    erase();
    hit_reset();
    if (COLS < 60 || LINES < 16) {
        put(0, 0, COLS, A_BOLD, "Terminal too small (%dx%d), need at least 60x16", COLS, LINES);
        refresh();
        return;
    }
    header(s, u);
    static int last_view = -1;
    if (u->view == VIEW_LOG && last_view != VIEW_LOG && !g_log.f) /* pick up new disks / interfaces */
        log_build_channels(s, u->log_skip);
    last_view = u->view;
    g_log.every = u->log_every;
    char note[200];
    int nerr, shown_err = 0;
    while (log_note_pop(note, sizeof note, &nerr)) /* e.g. a logged process exited */
        if (nerr || !shown_err) { /* an error stays over a later plain note */
            ui_message(u, nerr, "%s", note);
            shown_err |= nerr;
        }
    if (u->bell) {
        beep();
        u->bell = 0;
    }
    if (u->pd_open && u->view != u->pd_view) /* another view: the details page closes */
        u->pd_open = 0;
    static int was_events;
    if (was_events && u->view != VIEW_EVENTS)
        events_release();
    was_events = u->view == VIEW_EVENTS;
    if (u->view != VIEW_HW && (u->drv_open || u->cool_open)) { /* ... and the Hardware view's pages */
        u->drv_open = u->drv_confirm = 0;
        if (u->cool_open) {
            cooling_stop(); /* no CPU load left running behind your back */
            cooling_stopped_event(&s->health);
        }
        u->cool_open = 0;
    }
    switch (u->view) {
    case VIEW_DASH:
        if (u->pd_open)
            pdetail_view(s, u);
        else
            dashboard(s, u);
        break;
    case VIEW_LOG: log_view(s, u); break;
    case VIEW_PROCS:
        if (u->pd_open)
            pdetail_view(s, u);
        else
            draw_procs(s, u, 1, 0, LINES - 2, COLS, 1);
        break;
    case VIEW_HW:
        if (u->drv_open)
            drives_view(s, u);
        else if (u->cool_open)
            cool_view(s, u);
        else
            hardware(s, u);
        break;
    case VIEW_SETUP: setup_view(s, u); break;
    case VIEW_SVC: services_view(s, u); break;
    case VIEW_EVENTS: events_view(s, u); break;
    case VIEW_HIST: history_view(s, u); break;
    }
    footer(u);
    if (u->kill_open)
        kill_dialog(u);
    if (u->nice_open)
        nice_dialog(u);
    if (u->notice_open && g_notice.n)
        notice_dialog();
    if (u->help)
        help_overlay();
    refresh();
}

static void set_sort(ui_state_t *u, int s)
{
    if (u->sort == s)
        u->reverse ^= 1;
    else {
        u->sort = s;
        u->reverse = 0;
    }
}

int ui_key(sys_t *s, ui_state_t *u, int ch)
{
    if (ch == KEY_MOUSE)
        return mouse_event(s, u);
    if (ch <= -1000) { /* a click on a column heading */
        set_sort(u, -1000 - ch);
        return 1;
    }
    if (u->notice_open) {
        u->notice_open = 0;
        if (ch == 'v' && g_notice.log[0])
            lv_open(u, g_notice.log);
        return 1;
    }
    if (u->nice_open) {
        nice_key(u, ch);
        return 1;
    }
    if (u->filter_edit) { /* typing a filter: processes, or services in view 6 */
        char *f = u->view == VIEW_SVC ? u->svc_filter : u->view == VIEW_EVENTS ? u->ev_filter : u->filter;
        size_t cap = u->view == VIEW_SVC      ? sizeof u->svc_filter
                   : u->view == VIEW_EVENTS ? sizeof u->ev_filter
                                            : sizeof u->filter,
               len = strlen(f);
        if (ch == 27) {
            f[0] = '\0';
            u->filter_edit = 0;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            u->filter_edit = 0;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len)
                f[len - 1] = '\0';
        } else if (ch >= 32 && ch < 127 && len + 1 < cap) {
            f[len] = (char)ch;
            f[len + 1] = '\0';
        }
        u->sel_idx = u->scroll = 0;
        u->svc_sel = u->svc_scroll = 0;
        u->ev_sel = u->ev_scroll = 0;
        return 1;
    }
    if (u->kill_open) {
        kill_key(u, ch);
        return 1;
    }
    if (u->help) {
        u->help = 0;
        return 1;
    }
    if (u->pd_open && u->view == u->pd_view && pdetail_key(u, ch))
        return 1;
    if (u->view == VIEW_HW) {
        if (u->drv_open && drives_key(u, ch))
            return 1;
        if (u->cool_open && cool_key(s, u, ch))
            return 1;
        if (!u->drv_open && !u->cool_open && ch == 'D') {
            drives_open(s, u);
            return 1;
        }
        if (!u->drv_open && !u->cool_open && ch == 'C') {
            u->cool_open = 1;
            return 1;
        }
    }
    if (u->view == VIEW_SETUP && setup_key(u, ch))
        return 1;
    if (u->view == VIEW_LOG && log_view_key(s, u, ch))
        return 1;
    if (u->view == VIEW_SVC && ch != '/' && services_key(s, u, ch))
        return 1;
    if (u->view == VIEW_EVENTS && events_key(u, ch))
        return 1;
    if (u->view == VIEW_HIST && history_key(u, ch))
        return 1;

    static const double steps[] = { 0.25, 0.5, 1, 2, 3, 5, 10 };
    int nsteps = (int)(sizeof steps / sizeof steps[0]), si = 0;
    for (int i = 0; i < nsteps; i++)
        if (u->interval >= steps[i] - 1e-9)
            si = i;
    int page = MAX(1, u->page_rows - 1);
    switch (ch) {
    case 'q':
    case 'Q':
    case KEY_F(10): return 0;
    case '1': u->view = VIEW_DASH; break;
    case '2': u->view = VIEW_PROCS; break;
    case '3': u->view = VIEW_HW; break;
    case '4':
    case 's':
    case KEY_F(2): u->view = VIEW_SETUP; break;
    case '5': u->view = VIEW_LOG; break;
    case '6': u->view = VIEW_SVC; break;
    case '7': u->view = VIEW_EVENTS; break;
    case '8': u->view = VIEW_HIST; break;
    case 'l': log_toggle_system(s, u); break;
    case 'L': log_toggle_process(s, u); break;
    case '\t': u->view = (u->view + 1) % VIEW_COUNT; break;
    case KEY_BTAB: u->view = (u->view + VIEW_COUNT - 1) % VIEW_COUNT; break;
    case '\n':
    case '\r':
    case KEY_ENTER: /* everything about the selected process */
        if ((u->view == VIEW_PROCS || (u->view == VIEW_DASH && u->lay.show_procs)) && u->sel_pid)
            pdetail_open(u, u->sel_pid, u->sel_start);
        break;
    case 'x':
    case KEY_F(9): kill_open(u); break;
    case 'N': nice_open(s, u); break;
    case KEY_F(7): nice_step(s, u, -1); break;
    case KEY_F(8): nice_step(s, u, 1); break;
    case 'T':
    case KEY_F(5):
        u->tree ^= 1;
        if (!proc_view(u))
            u->view = VIEW_PROCS;
        ui_message(u, 0, u->tree ? "Process tree: children under the process that started them (T again: list)"
                                 : "Process list (T: tree)");
        break;
    case 'b': { /* what the per-core CPU bars show: usage -> temperature -> clock */
        int m = core_bar_metric(s, u->lay.core_bar);
        for (int k = 0; k < CORE_COUNT; k++) { /* skip what this CPU doesn't report */
            m = (m + 1) % CORE_COUNT;
            if (core_bar_metric(s, m) == m)
                break;
        }
        u->lay.core_bar = m;
        ui_message(u, 0, "CPU core bars show: %s   (b again for the next; Setup has more)", CORE_NAME[m]);
        break;
    }
    case 'c': set_sort(u, SORT_CPU); break;
    case 'm': set_sort(u, SORT_MEM); break;
    case 'd': set_sort(u, SORT_IO); break;
    case 'p': set_sort(u, SORT_PID); break;
    case 'n': set_sort(u, SORT_NAME); break;
    case 't': set_sort(u, SORT_TIME); break;
    case 'g': set_sort(u, SORT_GPU); break;
    case 'o': set_sort(u, SORT_FILES); break;
    case 'r': u->reverse ^= 1; break;
    case 'k': u->show_kthreads ^= 1; break;
    case 'f': u->follow ^= 1; break;
    case '/':
        u->filter_edit = 1;
        if (u->view != VIEW_DASH && u->view != VIEW_SVC && u->view != VIEW_EVENTS)
            u->view = VIEW_PROCS;
        break;
    case 27:
        u->filter[0] = '\0';
        u->svc_path[0] = '\0'; /* processes of one service -> all of them again */
        break;
    case '+':
    case '=': u->interval = steps[MIN(si + 1, nsteps - 1)]; break;
    case '-':
    case '_': u->interval = steps[MAX(si - 1, 0)]; break;
    case ' ':
        u->paused ^= 1;
        if (u->paused && (g_log.f || g_log.nplog))
            ui_message(u, 1, "Paused: logging is paused too until you press Space again");
        break;
    case '?':
    case 'h':
    case KEY_F(1): u->help = 1; break;
    case KEY_UP:
        if (u->view == VIEW_HW) u->hw_scroll--; else u->sel_delta--;
        break;
    case KEY_DOWN:
        if (u->view == VIEW_HW) u->hw_scroll++; else u->sel_delta++;
        break;
    case KEY_PPAGE:
        if (u->view == VIEW_HW) u->hw_scroll -= 10; else u->sel_delta -= page;
        break;
    case KEY_NPAGE:
        if (u->view == VIEW_HW) u->hw_scroll += 10; else u->sel_delta += page;
        break;
    case KEY_HOME:
        if (u->view == VIEW_HW) u->hw_scroll = 0; else u->sel_delta = -1000000;
        break;
    case KEY_END:
        if (u->view == VIEW_HW) u->hw_scroll = 1 << 20; else u->sel_delta = 1000000;
        break;
    }
    if (u->hw_scroll < 0)
        u->hw_scroll = 0;
    return 1;
}

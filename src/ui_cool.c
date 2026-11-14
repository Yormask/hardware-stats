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
/* ui_cool.c - Hardware view > C: the cooling check, live, and its verdict */
#define _GNU_SOURCE
#include "cooling.h"
#include "ui_int.h"

#include <math.h>
#include <string.h>

/* the key (or tab, as -1 - view) that would leave a running check, pressed once */
static int g_leave;

static int leaves(int ch)
{
    return (ch >= '1' && ch <= '8' && ch != '3') || ch == 's' || ch == '\t' || ch == KEY_BTAB || ch == 'q' ||
           ch == 'Q' || ch == KEY_F(2) || ch == KEY_F(10) || ch == 'T' || ch == KEY_F(5) || ch == '/';
}

static const char *what_key(int k, char *buf, size_t n)
{
    if (k < 0)
        return "Click the tab";
    const char *name = k == '\t'      ? "Tab"
                     : k == KEY_BTAB  ? "Shift+Tab"
                     : k == KEY_F(2)  ? "F2"
                     : k == KEY_F(5)  ? "F5"
                     : k == KEY_F(10) ? "F10"
                                      : NULL;
    if (name)
        sfmt(buf, n, "Press %s", name);
    else
        sfmt(buf, n, "Press %c", k);
    return buf;
}

/* 1: wait, the check is running (the first press only warns); 0: go ahead,
 * after stopping it if it ran. k: a key, or -1 - view for a click on a tab */
int cool_guard(sys_t *s, ui_state_t *u, int k)
{
    const cool_t *c = cooling_state();
    int busy = c->phase == COOL_BASE || c->phase == COOL_LOAD || c->phase == COOL_REST;
    if (!busy || !u->cool_open) {
        g_leave = 0;
        return 0;
    }
    if (g_leave == k) { /* again: stop it and go */
        g_leave = 0;
        cooling_stop();
        cooling_stopped_event(&s->health);
        ui_message(u, 0, "Cooling check stopped; the CPU load is off");
        return 0;
    }
    g_leave = k;
    char b[32];
    ui_message(u, 1, "The cooling check is running. %s again to stop the check and leave, any other key keeps it going.",
               what_key(k, b, sizeof b));
    return 1;
}

int cool_key(sys_t *s, ui_state_t *u, int ch)
{
    const cool_t *c = cooling_state();
    int busy = c->phase == COOL_BASE || c->phase == COOL_LOAD || c->phase == COOL_REST;
    if (!busy)
        g_leave = 0;
    if (busy && leaves(ch))
        return cool_guard(s, u, ch);
    if (busy && g_leave) { /* any other key: it goes on */
        g_leave = 0;
        ui_message(u, 0, "The cooling check goes on");
        return 1;
    }
    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (busy)
            return 1;
        if (cooling_start(s) != 0)
            ui_message(u, 1, "No CPU temperature sensor found: nothing to check with");
        u->paused = 0; /* the check needs the samples */
        return 1;
    case 27:
    case KEY_LEFT:
        if (busy) {
            cooling_stop();
            cooling_stopped_event(&s->health);
            ui_message(u, 0, "Cooling check stopped; the CPU load is off");
        }
        u->cool_open = 0;
        return 1;
    case ' ':
        return busy; /* no pausing in the middle of it */
    }
    return 0;
}

void cool_view(sys_t *s, ui_state_t *u)
{
    cooling_tick(s);
    const cool_t *c = cooling_state();
    int top = 1, H = LINES - 2, W = COLS;
    int busy = c->phase == COOL_BASE || c->phase == COOL_LOAD || c->phase == COOL_REST;
    char info[64] = "";
    if (busy)
        sfmt(info, sizeof info, "%.0f s left", MAX(0, cooling_phase_left()));
    box_(top, 0, H, W, "Cooling check", info);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2, y = iy;
    if (ih < 8)
        return;
    if (c->phase == COOL_IDLE || c->phase == COOL_CANCELLED) {
        put(y++, ix, iw, A_BOLD, "%s", "Does the cooling keep up with a long full load?");
        y++;
        y += wrap_text(y, ix, MIN(iw, 100), 4, A_NORMAL,
                       "All CPU threads are kept busy for a minute, then the CPU rests for half a minute. hsm watches the "
                       "temperature, the clock speed, the fans and throttling, says whether the cooling keeps up and "
                       "compares with the previous check: a few degrees more every few months means dust.");
        y++;
        y += wrap_text(y, ix, MIN(iw, 100), 3, A_GRAY,
                       "The load stops by itself 3 degrees before the CPU's limit. Close heavy programs first for a fair "
                       "result. On a laptop, plug in the charger. The same on the command line: hsm --cooling-check");
        y++;
        if (s->cpu.temp_c < 0)
            put(y++, ix, iw, CP(C_WARN) | A_BOLD, "%s", "No CPU temperature sensor found: the check can't run here.");
        else
            put(y++, ix, iw, CP(C_OK) | A_BOLD, "Enter: start   Esc: back      (CPU now %.0f%s)", s->cpu.temp_c, DEG);
        return;
    }
    static const char *const PH[] = { "", "Measuring the idle temperature", "Full load", "Resting", "Done", "" };
    attr_t pa = c->phase == COOL_LOAD ? CP(C_CRIT) | A_BOLD : c->phase == COOL_DONE ? CP(C_OK) | A_BOLD : A_BOLD;
    int xx = ix + put(y, ix, iw, pa, "%s", PH[c->phase]);
    if (busy)
        put(y, xx, ix + iw - xx, A_GRAY, "%s", "   (Esc stops it)");
    y += 2;
    char a[48];
    sfmt(a, sizeof a, "%.1f%s", c->phase == COOL_DONE ? c->end_temp : s->cpu.temp_c, DEG);
    kv(y, &(int){ ix }, ix + iw, temp_attr_lim(s->cpu.temp_c, c->limit - 10) | A_BOLD, "Now", "%s", a);
    y++;
    int kx = ix;
    if (c->nbase && c->phase != COOL_BASE)
        kv(y, &kx, ix + iw, A_BOLD, "Idle", "%.0f%s", c->base, DEG);
    if (c->peak >= 0)
        kv(y, &kx, ix + iw, temp_attr_lim(c->peak, c->limit - 10) | A_BOLD, "Peak", "%.0f%s after %.0f s", c->peak, DEG,
           c->peak_t);
    kv(y, &kx, ix + iw, A_GRAY, "Limit", "%.0f%s", c->limit, DEG);
    y++;
    kx = ix;
    if (s->cpu.mhz_avg > 0)
        kv(y, &kx, ix + iw, A_BOLD, "Clock", "%.0f MHz", s->cpu.mhz_avg);
    if (c->fan_base >= 0) {
        double f = -1;
        for (int i = 0; i < s->sens.n; i++)
            if (s->sens.s[i].type == SENS_FAN && s->sens.s[i].ok && !s->sens.s[i].hidden)
                f = MAX(f, s->sens.s[i].value);
        kv(y, &kx, ix + iw, A_BOLD, "Fastest fan", "%.0f RPM (idle %.0f)", f, c->fan_base);
    }
    if (s->health.thr_ok)
        kv(y, &kx, ix + iw, c->throttled ? CP(C_CRIT) | A_BOLD : CP(C_OK), "Throttled", "%s", c->throttled ? "yes" : "no");
    y += 2;
    /* the temperature over the check: idle | load | rest */
    int gh = MIN(8, iy + ih - y - 6);
    if (gh >= 3 && c->ncurve > 1) {
        int gw = MIN(iw - 8, c->ncurve);
        int from = c->ncurve - gw;
        double lo = 1e9, hi = -1e9;
        for (int k = from; k < c->ncurve; k++) {
            lo = MIN(lo, (double)c->curve[k]);
            hi = MAX(hi, (double)c->curve[k]);
        }
        lo = floor(lo / 10) * 10;
        hi = MAX(hi, lo + 20);
        put(y, ix, 6, A_GRAY, "%.0f%s", hi, U ? "°" : "");
        put(y + gh - 1, ix, 6, A_GRAY, "%.0f%s", lo, U ? "°" : "");
        int hl = c->load_mark > from ? c->load_mark - from : -1;
        graph_cols(y, ix + 6, gh, gw, c->curve + from, lo, hi, CP(C_IN), -1, 0);
        if (hl >= 0)
            put(y + gh, ix + 6 + hl, 10, A_GRAY, "%s load", U ? "↑" : "^");
        if (c->rest_mark > from && c->rest_mark - from < gw - 4)
            put(y + gh, ix + 6 + c->rest_mark - from, 10, A_GRAY, "%s rest", U ? "↑" : "^");
        y += gh + 2;
    }
    if (c->phase == COOL_DONE) {
        attr_t va = c->level == 2 ? CP(C_CRIT) | A_BOLD : c->level ? CP(C_WARN) | A_BOLD : CP(C_OK) | A_BOLD;
        y += wrap_text(y, ix, iw, MAX(0, iy + ih - 1 - y), va, c->verdict);
        if (!isnan(c->mhz_first) && !isnan(c->mhz_last) && y < iy + ih - 1)
            put(y++, ix, iw, A_NORMAL, "Clock under load: %.0f MHz at first, %.0f MHz at the end%s.", c->mhz_first,
                c->mhz_last, c->mhz_last < c->mhz_first * 0.85 ? " (a power or heat limit stepped in)" : "");
        if (c->compare[0] && y < iy + ih - 1)
            put(y++, ix, iw, A_NORMAL, "%s.", c->compare);
        if (y < iy + ih - 1)
            put(y + 1, ix, iw, A_GRAY, "%s", "Enter: check again   Esc: back      (results are kept in cooling.log)");
    }
}

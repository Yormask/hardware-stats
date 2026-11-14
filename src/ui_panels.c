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
/* ui_panels.c - dashboard panels and the configurable two-column layout */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ================================================================== CPU */

/* Each core gets one row: a bar for the metric picked in Setup (key b
 * cycles it) with its value, then the other two values as plain numbers:
 *
 *   #  usage              clock  temp
 *   0  ██████▌······  48%  3.6G   54°
 *
 * A grey heading names the columns. With many cores (or a narrow panel)
 * the extra values go first, then the heading. */
typedef struct {
    int bar;     /* CORE_* shown as the bar */
    int val[2];  /* other values printed after it */
    int nval;
    int idw;     /* digits of the highest CPU number */
    int head;    /* draw the heading row */
} coreview_t;

static int g_core_max = 8; /* rows allowed for per-core bars, set every frame */

/* per-core temperature, -1 when the CPU has no sensor per core */
static double core_temp(const sys_t *s, int i)
{
    const cpu_t *c = &s->cpu;
    int id = c->id[i];
    if (id < 0 || id >= MAX_CPUS)
        return -1;
    int core = c->core_id[id] >= 0 ? c->core_id[id] : id;
    return sensors_core_temp(&s->sens, c->pkg_id[id], core);
}

static int core_has(const sys_t *s, int m)
{
    for (int i = 0; i < s->cpu.ncpu; i++)
        if (m == CORE_USAGE || (m == CORE_CLOCK && s->cpu.mhz[i] > 0) || (m == CORE_TEMP && core_temp(s, i) >= 0))
            return 1;
    return 0;
}

/* the metric the bars can show, given what this CPU reports */
int core_bar_metric(const sys_t *s, int want)
{
    return want >= 0 && want < CORE_COUNT && core_has(s, want) ? want : CORE_USAGE;
}

/* cell width needed: number + bar + bar value + extra values (+ gap) */
static int core_cell_w(const coreview_t *v, int bw)
{
    return v->idw + 1 + bw + 1 + 4 + v->nval * 6 + 3;
}

static void core_view(const sys_t *s, const layout_t *l, coreview_t *v)
{
    memset(v, 0, sizeof *v);
    v->bar = core_bar_metric(s, l->core_bar);
    static const int order[CORE_COUNT] = { CORE_USAGE, CORE_CLOCK, CORE_TEMP };
    if (l->core_values)
        for (int k = 0; k < CORE_COUNT; k++)
            if (order[k] != v->bar && core_has(s, order[k]))
                v->val[v->nval++] = order[k];
    int maxid = 0;
    for (int i = 0; i < s->cpu.ncpu; i++)
        maxid = MAX(maxid, s->cpu.id[i]);
    v->idw = maxid >= 100 ? 3 : maxid >= 10 ? 2 : 1;
}

/* columns of cores; fewer extra values when even one column is too narrow */
static int core_cols(coreview_t *v, int iw, int ncpu, int max_rows)
{
    while (v->nval && core_cell_w(v, 6) > iw + 3)
        v->nval--;
    if (ncpu <= 1)
        return 1;
    max_rows = MAX(max_rows, 1);
    int cols = (ncpu + max_rows - 1) / max_rows;
    int pref = MAX((iw + 3) / core_cell_w(v, 14), 1); /* roomy bars when there is space */
    cols = MIN(MAX(cols, pref), ncpu);
    while (cols > 1 && (iw + 3) / cols < core_cell_w(v, 5)) { /* too narrow: drop values, then columns */
        if (v->nval)
            v->nval--;
        else
            cols--;
    }
    return cols;
}

static int core_rows(const sys_t *s, const layout_t *l, int iw, int max_rows)
{
    coreview_t v;
    core_view(s, l, &v);
    int cols = core_cols(&v, iw, s->cpu.ncpu, max_rows);
    return (s->cpu.ncpu + cols - 1) / cols;
}

/* bar fill (0-100), value text and colours of one metric of one core */
static void core_metric(const sys_t *s, int m, int i, double *bpct, char *txt, size_t n, attr_t *ba, attr_t *ta)
{
    const cpu_t *c = &s->cpu;
    int id = c->id[i];
    if (m == CORE_CLOCK) {
        double mhz = c->mhz[i], mx = id >= 0 && id < MAX_CPUS && c->mhz_hw[id] > 0 ? c->mhz_hw[id] : c->mhz_max;
        *bpct = mhz > 0 && mx > 0 ? 100.0 * mhz / mx : 0;
        if (mhz > 0)
            sfmt(txt, n, "%.1fG", mhz / 1000.0);
        else
            str_copy(txt, n, "-");
        *ba = gradient_on() ? 0 : CP(C_IN);
        *ta = mhz > 0 ? A_NORMAL : A_GRAY;
    } else if (m == CORE_TEMP) {
        double t = core_temp(s, i);
        *bpct = t > 0 ? t : 0; /* the bar spans 0-100 °C */
        if (t >= 0)
            sfmt(txt, n, "%.0f%s", t, U ? "°" : "C");
        else
            str_copy(txt, n, "-");
        *ba = gradient_on() ? 0 : bar_temp_attr(t);
        *ta = t >= 0 ? temp_attr(t) : A_GRAY;
    } else {
        *bpct = c->pct[i];
        sfmt(txt, n, "%.0f%%", c->pct[i]);
        *ba = 0;
        *ta = pct_attr(c->pct[i]);
    }
}

static const char *const CORE_HEAD[CORE_COUNT] = { "usage", "temp", "clock" };

static void core_head(const coreview_t *v, int y, int x, int cw)
{
    int bw = cw - core_cell_w(v, 0);
    if (bw < 2)
        return;
    put(y, x, v->idw, A_GRAY, "%*s", v->idw, "#");
    int bx = x + v->idw + 1;
    put(y, bx, bw, A_GRAY, "%s", v->bar == CORE_TEMP ? "temperature" : v->bar == CORE_CLOCK ? "clock speed" : "usage");
    int vx = bx + bw + 1 + 4;
    for (int j = 0; j < v->nval; j++, vx += 6)
        put(y, vx + 6 - (int)strlen(CORE_HEAD[v->val[j]]), (int)strlen(CORE_HEAD[v->val[j]]), A_GRAY, "%s",
            CORE_HEAD[v->val[j]]);
}

static void core_cell(const sys_t *s, const coreview_t *v, int i, int y, int x, int cw)
{
    double bp;
    char txt[16];
    attr_t ba, ta;
    int bw = cw - core_cell_w(v, 0);
    put(y, x, v->idw, A_GRAY, "%*d", v->idw, s->cpu.id[i]);
    int bx = x + v->idw + 1;
    core_metric(s, v->bar, i, &bp, txt, sizeof txt, &ba, &ta);
    if (bw >= 2)
        bar(y, bx, bw, bp, ba);
    else
        bw = 0;
    /* values are right-aligned by display columns: "54°" is 3 columns but 4 bytes */
    int vx = bx + bw + 1;
    put(y, vx + 4 - utf8_cols(txt), 4, ta | A_BOLD, "%s", txt);
    vx += 4;
    for (int j = 0; j < v->nval; j++, vx += 6) {
        core_metric(s, v->val[j], i, &bp, txt, sizeof txt, &ba, &ta);
        put(y, vx + 6 - utf8_cols(txt), utf8_cols(txt), ta, "%s", txt);
    }
}

static int cpu_need(sys_t *s, ui_state_t *u, int w)
{
    return 4 + MIN(core_rows(s, &u->lay, w - 4, g_core_max), g_core_max) + 1; /* + column heading */
}

static void panel_cpu(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    cpu_t *c = &s->cpu;
    char info[256] = "", v[32], a[16], b[16], d[16];
    app(info, sizeof info, "%s", c->model);
    if (c->threads)
        app(info, sizeof info, "%s%dC/%dT", SEP, c->cores, c->threads);
    box_(y, x, h, w, "CPU", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0 || iw <= 10)
        return;

    /* row 0: total + temperature + package power */
    int right = 0;
    char tbuf[16] = "", pbuf[16] = "";
    if (c->temp_c >= 0) {
        sfmt(tbuf, sizeof tbuf, "%.0f%s", c->temp_c, DEG);
        right += utf8_cols(tbuf) + 2;
    }
    if (s->power.pkg_w >= 0) {
        sfmt(pbuf, sizeof pbuf, "%.1f W", s->power.pkg_w);
        right += utf8_cols(pbuf) + 2;
    }
    sfmt(v, sizeof v, "%5.1f%%", c->total);
    meter(iy, ix, iw - right, "Total", 6, c->total, v, 6, 0);
    int xx = ix + iw - right;
    if (tbuf[0])
        xx += 2 + put(iy, xx + 2, ix + iw - xx - 2, temp_attr(c->temp_c) | A_BOLD, "%s", tbuf);
    if (pbuf[0])
        put(iy, xx + 2, ix + iw - xx - 2, CP(C_WARN) | A_BOLD, "%s", pbuf);
    r++;

    /* row 1: breakdown */
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_si(c->ctxt_rate, a, sizeof a);
        fmt_si(c->intr_rate, b, sizeof b);
        fmt_si(c->fork_rate, d, sizeof d);
        kv(iy + r, &kx, km, A_BOLD, "usr", "%.1f%%", c->user);
        kv(iy + r, &kx, km, A_BOLD, "sys", "%.1f%%", c->sys);
        kv(iy + r, &kx, km, c->iowait >= 5 ? CP(C_WARN) : A_BOLD, "iowait", "%.1f%%", c->iowait);
        if (c->mhz_avg > 0)
            kv(iy + r, &kx, km, A_BOLD, "clock", "%.2f GHz", c->mhz_avg / 1000.0);
        /* what holds the clock down right now, like the GPU's */
        const health_t *H = &s->health;
        if (H->thr_last > 0 && time(NULL) - H->thr_last < 10)
            kv(iy + r, &kx, km, CP(C_WARN) | A_BOLD, "slowing", "%s", "heat");
        else if (H->pl_now)
            kv(iy + r, &kx, km, A_BOLD, "slowing", "%s", H->pl_now == PL_SHORT ? "boost limit" : "power limit");
        kv(iy + r, &kx, km, A_BOLD, "load", "%.2f %.2f %.2f", c->load[0], c->load[1], c->load[2]);
        if (c->steal > 0.05)
            kv(iy + r, &kx, km, CP(C_WARN), "steal", "%.1f%%", c->steal);
        kv(iy + r, &kx, km, A_BOLD, "ctx/s", "%s", a);
        kv(iy + r, &kx, km, A_BOLD, "irq/s", "%s", b);
        kv(iy + r, &kx, km, A_BOLD, "forks/s", "%s", d);
        r++;
    }

    /* cores at the bottom (heading first), the history graph fills what is left */
    coreview_t cv;
    core_view(s, &u->lay, &cv);
    int cols = core_cols(&cv, iw, c->ncpu, g_core_max);
    int rows_total = (c->ncpu + cols - 1) / cols;
    int avail = ih - r;
    int rows_vis = MIN(rows_total, avail);
    cv.head = avail > rows_vis; /* a spare row: name the columns */
    int gh = avail - rows_vis - cv.head;
    if (gh > 0)
        graph(iy + r, ix, gh, iw, &s->h_cpu, 100, 0, 0);
    int cy = iy + r + MAX(gh, 0);
    int cw = (iw + 3) / cols; /* + 3: the last column needs no gap after it */
    if (cv.head) {
        for (int col = 0; col < cols; col++)
            core_head(&cv, cy, ix + col * cw, col == cols - 1 ? iw + 3 - col * cw : cw);
        cy++;
    }
    for (int i = 0; i < c->ncpu; i++) {
        int col = i / rows_total, row = i % rows_total;
        if (row < rows_vis)
            core_cell(s, &cv, i, cy + row, ix + col * cw, col == cols - 1 ? iw + 3 - col * cw : cw);
    }
}

/* ================================================================== GPU */

static int gpu_block(const gpu_t *g, int multi)
{
    return multi + 1 + (g->mem_total > 0) + (g->clk_max_mhz > 0) + 1;
}

static int gpu_need(sys_t *s, ui_state_t *u, int w)
{
    if (!s->gpus.n)
        return 3;
    int n = 2;
    for (int i = 0; i < s->gpus.n; i++)
        n += gpu_block(&s->gpus.g[i], s->gpus.n > 1);
    return n;
}

/* "heat", "power limit", "heat, power limit": why the GPU is held back now */
static const char *slow_words(int bits)
{
    return bits == (GS_HEAT | GS_POWER) ? "heat, power limit" : bits & GS_HEAT ? "heat" : bits & GS_POWER ? "power limit" : "";
}

static void gpu_details(int y, int x, int w, const gpu_t *g)
{
    int xx = x, xm = x + w;
    if (g->temp_c >= 0)
        kv(y, &xx, xm, temp_attr_lim(g->temp_c, g->temp_lim) | A_BOLD, "", "%.0f%s", g->temp_c, DEG);
    if (g->slow_now) /* the driver says why it isn't at full speed */
        kv(y, &xx, xm, g->slow_now & GS_HEAT ? CP(C_WARN) | A_BOLD : A_BOLD, "slowing:", "%s", slow_words(g->slow_now));
    if (g->power_w >= 0) {
        if (g->power_cap_w > 0)
            kv(y, &xx, xm, CP(C_WARN) | A_BOLD, "", "%.1f/%.0f W", g->power_w, g->power_cap_w);
        else
            kv(y, &xx, xm, CP(C_WARN) | A_BOLD, "", "%.1f W", g->power_w);
    }
    if (g->clk_mhz >= 0 && g->clk_max_mhz <= 0) /* with a known max it gets its own bar */
        kv(y, &xx, xm, A_BOLD, "clock", "%.0f MHz", g->clk_mhz);
    if (g->mem_clk_mhz > 0)
        kv(y, &xx, xm, A_BOLD, "mem", "%.0f MHz", g->mem_clk_mhz);
    if (g->fan_pct >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f%%", g->fan_pct);
    else if (g->fan_rpm >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f rpm", g->fan_rpm);
    for (int i = 0; i < g->neng; i++)
        kv(y, &xx, xm, pct_attr(g->eng[i].pct), g->eng[i].name, "%.0f%%", g->eng[i].pct);
    if (g->enc_pct > 0) /* video: only while it is used */
        kv(y, &xx, xm, pct_attr(g->enc_pct), "encode", "%.0f%%", g->enc_pct);
    if (g->dec_pct > 0)
        kv(y, &xx, xm, pct_attr(g->dec_pct), "decode", "%.0f%%", g->dec_pct);
    if (g->pcie_gen > 0 && g->pcie_width > 0)
        kv(y, &xx, xm, A_BOLD, "PCIe", "%d.0 x%d", g->pcie_gen, g->pcie_width);
    char held[64];
    for (int i = 0; i < g->nscr && gpu_screen_holds(g, held, sizeof held)[0]; i++)
        if (g->scr[i].on) { /* it can't power off while it draws a screen */
            kv(y, &xx, xm, CP(C_INFO), "awake for", "the screen on %s", g->scr[i].conn);
            break;
        }
}

static void panel_gpu(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    gpus_t *G = &s->gpus;
    char info[128] = "";
    if (G->n == 1)
        str_copy(info, sizeof info, G->g[0].name);
    else if (G->n > 1)
        sfmt(info, sizeof info, "%d GPUs", G->n);
    box_(y, x, h, w, "GPU", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!G->n) {
        put(iy, ix, iw, A_GRAY, "No GPU found in /sys/class/drm");
        return;
    }
    int multi = G->n > 1, graph_gpu = -1;
    for (int k = 0; k < G->n && r < ih; k++) {
        gpu_t *g = &G->g[k];
        if (multi)
            put(iy + r++, ix, iw, A_BOLD, "%d %s", k, g->name);
        if (r >= ih)
            break;
        if (g->nv_state == NV_ASLEEP || g->nv_state == NV_IDLE) {
            /* powered off, or left alone so it can be: no figures without waking it */
            int xx = ix + put(iy + r, ix, iw, CP(C_INFO), "Load  ");
            put(iy + r++, xx, ix + iw - xx, g->note[0] ? CP(C_WARN) : A_GRAY, "%s",
                g->note[0] ? g->note
                : g->nv_state == NV_ASLEEP ? "asleep: powered off to save power"
                                           : "idle: hsm let go of it so it can power off");
            if (r < ih && g->mem_total > 0) /* the same rows as when it's awake */
                meter(iy + r++, ix, iw, "VRAM", 6, 0, "-", 11, A_GRAY);
            if (r < ih && g->clk_max_mhz > 0)
                meter(iy + r++, ix, iw, "Clock", 6, 0, "-", 15, A_GRAY);
            if (r < ih)
                r++;
            continue;
        }
        if (g->util >= 0) {
            char v[16];
            sfmt(v, sizeof v, "%5.1f%%", g->util);
            meter(iy + r++, ix, iw, g->util_est ? "Load~" : "Load", 6, g->util, v, 6, 0);
            if (graph_gpu < 0)
                graph_gpu = k;
        } else {
            int xx = ix + put(iy + r, ix, iw, CP(C_INFO), "Load  ");
            put(iy + r++, xx, ix + iw - xx, CP(C_WARN), "%s", g->note[0] ? g->note : "n/a");
        }
        if (r < ih && g->mem_total > 0) {
            char a[16], b[16], v[40];
            fmt_bytes(g->mem_used, a, sizeof a);
            fmt_bytes(g->mem_total, b, sizeof b);
            sfmt(v, sizeof v, "%s/%s", a, b);
            meter(iy + r++, ix, iw, "VRAM", 6, 100.0 * g->mem_used / g->mem_total, v, 11, 0);
        }
        if (r < ih && g->clk_max_mhz > 0) {
            char v[40];
            double cpct = g->clk_mhz > 0 ? 100.0 * g->clk_mhz / g->clk_max_mhz : 0;
            if (g->clk_mhz >= 0)
                sfmt(v, sizeof v, "%.0f/%.0f MHz", g->clk_mhz, g->clk_max_mhz);
            else
                sfmt(v, sizeof v, "?/%.0f MHz", g->clk_max_mhz);
            meter(iy + r++, ix, iw, "Clock", 6, cpct, v, 15, gradient_on() ? 0 : CP(C_IN));
        }
        if (r < ih)
            gpu_details(iy + r++, ix, iw, g);
    }
    if (graph_gpu >= 0 && r < ih)
        graph(iy + r, ix, ih - r, iw, &s->h_gpu[graph_gpu], 100, 0, 0);
}

/* =============================================================== Memory */

static int mem_need(sys_t *s, ui_state_t *u, int w)
{
    return 6 + (s->mem.nzram > 0); /* zram: how full, how well it compresses */
}

static void panel_mem(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    mem_t *m = &s->mem;
    char a[16], b[16], v[48], info[64];
    fmt_bytes((double)m->total, a, sizeof a);
    sfmt(info, sizeof info, "%s total", a);
    box_(y, x, h, w, "Memory", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;

    double pct = m->total ? 100.0 * (double)m->used / (double)m->total : 0;
    fmt_bytes((double)m->used, a, sizeof a);
    fmt_bytes((double)m->total, b, sizeof b);
    sfmt(v, sizeof v, "%s/%s", a, b);
    meter(iy + r++, ix, iw, "RAM", 5, pct, v, 11, 0);

    if (r < ih) {
        if (m->swap_total) {
            fmt_bytes((double)m->swap_used, a, sizeof a);
            fmt_bytes((double)m->swap_total, b, sizeof b);
            sfmt(v, sizeof v, "%s/%s", a, b);
            meter(iy + r, ix, iw, "Swap", 5, 100.0 * (double)m->swap_used / (double)m->swap_total, v, 11, 0);
        } else {
            put(iy + r, ix, 5, CP(C_INFO), "Swap");
            put(iy + r, ix + 5, iw - 5, A_GRAY, "none");
        }
        r++;
    }
    if (m->nzram && r < ih) { /* compressed RAM: all zram devices together */
        unsigned long long used = 0, size = 0, orig = 0, compr = 0, ram = 0;
        const char *algo = "";
        for (int i = 0; i < m->nswap; i++)
            if (m->swap[i].zram) {
                used += m->swap[i].used;
                size += m->swap[i].size;
                orig += m->swap[i].orig;
                compr += m->swap[i].compr;
                ram += m->swap[i].ram;
                if (!algo[0])
                    algo = m->swap[i].algo;
            }
        int kx = ix, km = ix + iw;
        fmt_bytes((double)used, a, sizeof a);
        fmt_bytes((double)size, b, sizeof b);
        kv(iy + r, &kx, km, A_BOLD, "zram", "%s/%s", a, b);
        if (compr > 0 && orig > 0)
            kv(iy + r, &kx, km, A_BOLD, "", "%.1fx smaller", (double)orig / (double)compr);
        if (ram > 0) {
            fmt_bytes((double)ram, a, sizeof a);
            kv(iy + r, &kx, km, A_BOLD, "takes", "%s", a);
        }
        if (algo[0])
            kv(iy + r, &kx, km, A_GRAY, "", "%s", algo);
        r++;
    }
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_bytes((double)m->buffcache, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "cache", "%s", a);
        fmt_bytes((double)m->avail, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "avail", "%s", a);
        fmt_bytes((double)m->shmem, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "shm", "%s", a);
        fmt_bytes((double)m->dirty, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "dirty", "%s", a);
        r++;
    }
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_rate(m->swapin_bps, a, sizeof a);
        fmt_rate(m->swapout_bps, b, sizeof b);
        kv(iy + r, &kx, km, m->swapin_bps > 0 ? CP(C_WARN) : A_BOLD, "swap in", "%s", a);
        kv(iy + r, &kx, km, m->swapout_bps > 0 ? CP(C_WARN) : A_BOLD, "out", "%s", b);
        fmt_si(m->majflt_rate, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "majflt/s", "%s", a);
        r++;
    }
    if (r < ih)
        graph(iy + r, ix, ih - r, iw, &s->h_mem, 100, 0, 0);
}

/* ================================================================ Power */

typedef struct {
    char label[64], val[32];
    attr_t va;
    int sub;
    int drop; /* short of room, the highest goes first: 3 cores and iGPU, 2 the AC adapter, 1 a sleeping GPU */
} pline_t;

static int pl_add(pline_t *L, int n, int max, attr_t va, int sub, const char *label, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
static int pl_add(pline_t *L, int n, int max, attr_t va, int sub, const char *label, const char *fmt, ...)
{
    if (n >= max)
        return n;
    str_copy(L[n].label, sizeof L[n].label, label);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L[n].val, sizeof L[n].val, fmt, ap);
    va_end(ap);
    L[n].va = va;
    L[n].sub = sub;
    L[n].drop = sub ? 3 : 0;
    return n + 1;
}

static int build_power(sys_t *s, pline_t *L, int max)
{
    power_t *p = &s->power;
    int n = 0;
    if (p->pkg_w >= 0) {
        n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, "CPU package", "%.1f W", p->pkg_w);
        if (p->core_w >= 0)
            n = pl_add(L, n, max, A_BOLD, 1, "cores", "%.1f W", p->core_w);
        if (p->uncore_w >= 0)
            n = pl_add(L, n, max, A_BOLD, 1, "iGPU / uncore", "%.1f W", p->uncore_w);
    } else if (p->rapl_denied) {
        n = pl_add(L, n, max, CP(C_WARN), 0, "CPU (RAPL)", "%s", "needs root");
    }
    if (p->dram_w >= 0)
        n = pl_add(L, n, max, A_BOLD, 0, "DRAM", "%.1f W", p->dram_w);
    if (p->psys_w >= 0 && !p->psys_bad) /* not when the firmware left it unconnected */
        n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, "Platform (psys)", "%.1f W", p->psys_w);
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (g->power_w < 0 || (g->integrated && !strcmp(g->vendor, "Intel") && p->uncore_w >= 0))
            continue; /* Intel iGPU already shown as uncore */
        char lab[48];
        sfmt(lab, sizeof lab, "GPU%d %s", i, g->vendor);
        if (g->nv_state == NV_ASLEEP || g->nv_state == NV_IDLE) {
            n = pl_add(L, n, max, A_GRAY, 0, lab, "%s", g->nv_state == NV_ASLEEP ? "asleep" : "idle");
            L[n - 1].drop = 1;
        } else if (g->power_cap_w > 0)
            n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, lab, "%.1f/%.0f W", g->power_w, g->power_cap_w);
        else
            n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, lab, "%.1f W", g->power_w);
    }
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        char lab[64] = "", tl[24] = "";
        app(lab, sizeof lab, "%s", b->name);
        if (b->capacity >= 0)
            app(lab, sizeof lab, " %.0f%%", b->capacity);
        app(lab, sizeof lab, " %s", b->status);
        if (b->time_left > 0) {
            fmt_duration(b->time_left, tl, sizeof tl);
            app(lab, sizeof lab, "%s%s", SEP, tl);
        }
        if (b->health >= 0)
            app(lab, sizeof lab, "%shealth %.0f%%", SEP, b->health);
        if (b->lim_end > 0)
            app(lab, sizeof lab, "%slimit %d%%", SEP, b->lim_end);
        attr_t va = !strcmp(b->status, "Discharging") ? CP(C_WARN) | A_BOLD : CP(C_OK) | A_BOLD;
        if (b->watts >= 0)
            n = pl_add(L, n, max, va, 0, lab, "%.1f W", b->watts);
        else
            n = pl_add(L, n, max, va, 0, lab, "%s", "");
    }
    if (p->ac_online >= 0 && p->nbat) {
        n = pl_add(L, n, max, A_BOLD, 0, "AC adapter", "%s", p->ac_online ? "online" : "offline");
        L[n - 1].drop = 2;
    }
    if (!n)
        n = pl_add(L, n, max, A_GRAY, 0, "No power sensors found", "%s", "");
    return n;
}

static int power_need(sys_t *s, ui_state_t *u, int w)
{
    pline_t L[16];
    return 2 + build_power(s, L, 16);
}

static void panel_power(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    pline_t L[16];
    int n = build_power(s, L, 16);
    char info[64] = "";
    if (s->total_w >= 0)
        sfmt(info, sizeof info, "%s%.1f W (%s)", U ? "≈ " : "~", s->total_w, s->total_w_src);
    else if (s->total_why[0]) /* the GPUs alone are no total */
        sfmt(info, sizeof info, "total %s", s->total_why);
    box_(y, x, h, w, "Power", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    while (n > ih) { /* short of room: cores, iGPU, the AC adapter and a sleeping GPU go before the battery */
        int d = -1;
        for (int i = n - 1; i >= 0; i--)
            if (L[i].drop > 0 && (d < 0 || L[i].drop > L[d].drop))
                d = i;
        if (d < 0)
            break;
        memmove(&L[d], &L[d + 1], sizeof L[0] * (size_t)(n - d - 1));
        n--;
    }
    for (int i = 0; i < n && r < ih; i++, r++) {
        int vw = utf8_cols(L[i].val);
        if (L[i].sub)
            put(iy + r, ix, iw - vw - 1, A_GRAY, "  %s", L[i].label);
        else
            put(iy + r, ix, iw - vw - 1, CP(C_INFO), "%s", L[i].label);
        put(iy + r, ix + iw - vw, vw, L[i].va ? L[i].va : A_NORMAL, "%s", L[i].val);
    }
    if (r < ih) {
        double mx = hist_max(&s->h_pw, graph_span(iw)) * 1.1;
        graph(iy + r, ix, ih - r, iw, &s->h_pw, mx, CP(C_WARN), 0);
        if (ih - r >= 2 && mx > 0 && g_lay->graph_style != GRAPH_OFF)
            put(iy + r, ix, iw, A_GRAY, "%s%.0f W", s->pw_partial ? "GPU " : "", mx);
    }
}

/* ============================================================ Disk / Net */

static int disk_need(sys_t *s, ui_state_t *u, int w)
{
    return 2 + MAX(MIN(s->disks.n, 4), 1);
}

static void panel_disk(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    disks_t *D = &s->disks;
    char a[16], b[16], info[64];
    fmt_rate(D->rd_bps, a, sizeof a);
    fmt_rate(D->wr_bps, b, sizeof b);
    sfmt(info, sizeof info, "R %s  W %s", a, b);
    box_(y, x, h, w, "Disk I/O", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!D->n)
        put(iy + r++, ix, iw, A_GRAY, "No physical drives found");
    int nw = iw >= 44 ? 10 : 8;
    for (int i = 0; i < D->n && r < ih && i < 4; i++, r++) {
        disk_t *d = &D->d[i];
        int xx = ix, xm = ix + iw;
        put(iy + r, xx, nw, A_BOLD, "%s", d->name);
        xx += nw;
        fmt_rate(d->rd_bps, a, sizeof a);
        fmt_rate(d->wr_bps, b, sizeof b);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "R ");
        xx += put(iy + r, xx, xm - xx, d->rd_bps > 0 ? CP(C_IN) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  W ");
        xx += put(iy + r, xx, xm - xx, d->wr_bps > 0 ? CP(C_OUT) | A_BOLD : A_GRAY, "%7s", b);
        if (xm - xx >= 6)
            xx += put(iy + r, xx, xm - xx, pct_attr(d->util), " %4.0f%%", d->util);
        if (xm - xx >= 13) { /* average time per request: slow or failing drives show here first */
            double lat = MAX(d->rd_lat, d->wr_lat);
            xx += put(iy + r, xx, xm - xx, lat >= 200 ? CP(C_CRIT) | A_BOLD : lat >= 50 ? CP(C_WARN) : A_GRAY,
                      lat >= 10 ? "  %5.0f ms" : "  %5.1f ms", lat);
        }
        if (xm - xx >= 14) {
            fmt_si(d->rd_iops + d->wr_iops, a, sizeof a);
            put(iy + r, xx, xm - xx, A_GRAY, "  %s IOPS", a);
        }
    }
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_dr, &s->h_dw, CP(C_IN), CP(C_OUT), "R", "W");
}

static int net_pick(sys_t *s, int *idx, int max)
{
    int n = 0;
    for (int pass = 0; pass < 3 && n < max; pass++)
        for (int i = 0; i < s->net.n && n < max; i++) {
            netif_t *f = &s->net.i[i];
            int ok = 0;
            if (pass == 0) /* physical NICs that are up */
                ok = !f->is_virtual && f->up;
            else if (pass == 1) /* VPNs, bridges with traffic (skip veth noise) */
                ok = f->is_virtual && f->up && f->rx_total + f->tx_total > 0 && strncmp(f->name, "veth", 4);
            else if (n == 0) /* nothing yet: show anything */
                ok = 1;
            if (ok)
                idx[n++] = i;
        }
    return n;
}

static int net_need(sys_t *s, ui_state_t *u, int w)
{
    int idx[8];
    return 2 + MAX(net_pick(s, idx, 4), 1) + (s->net.q.on ? 1 : 0);
}

/* the network check's row: delay to the router and the internet, and losses */
static void net_ping_row(const netq_t *q, int y, int x, int w)
{
    int xx = x, xm = x + w;
    xx += put(y, xx, xm - xx, CP(C_INFO), "%s", "ping  ");
    if (q->err) {
        put(y, xx, xm - xx, CP(C_WARN), "%s", q->err == EACCES || q->err == EPERM
                                                 ? "not allowed (sysctl net.ipv4.ping_group_range)" : strerror(q->err));
        return;
    }
    static const char *const nm[NQ_COUNT] = { "router", "internet" };
    for (int k = 0; k < NQ_COUNT; k++) {
        const nqt_t *t = &q->t[k];
        if (k)
            xx += put(y, xx, xm - xx, A_GRAY, "%s", SEP);
        xx += put(y, xx, xm - xx, A_GRAY, "%s ", nm[k]);
        if (t->noroute)
            xx += put(y, xx, xm - xx, CP(C_WARN), "%s", "no network");
        else if (!t->addr[0])
            xx += put(y, xx, xm - xx, A_GRAY, "%s", k == NQ_ROUTER ? "none" : "...");
        else if (t->rtt_ms >= 0 && t->rtt_ms < 0.05)
            xx += put(y, xx, xm - xx, A_BOLD, "%s", "<0.1 ms");
        else if (t->rtt_ms >= 0)
            xx += put(y, xx, xm - xx, t->trouble ? CP(C_WARN) | A_BOLD : A_BOLD, t->rtt_ms < 10 ? "%.1f ms" : "%.0f ms",
                      t->rtt_ms);
        else if (t->loss_pct >= 0)
            xx += put(y, xx, xm - xx, CP(C_CRIT) | A_BOLD, "%s", "lost");
        else
            xx += put(y, xx, xm - xx, A_GRAY, "%s", "...");
        if (t->loss_pct > 0)
            xx += put(y, xx, xm - xx, t->loss_pct >= 10 ? CP(C_CRIT) | A_BOLD : CP(C_WARN), " (%.0f%% lost)", t->loss_pct);
    }
}

static void panel_net(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    int idx[8], nn = net_pick(s, idx, 4);
    char a[16], b[16], info[64];
    fmt_rate(s->net.rx_bps, a, sizeof a);
    fmt_rate(s->net.tx_bps, b, sizeof b);
    sfmt(info, sizeof info, "%s %s  %s %s", U ? "↓" : "in", a, U ? "↑" : "out", b);
    box_(y, x, h, w, "Network", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!nn)
        put(iy + r++, ix, iw, A_GRAY, "No network interfaces");
    int nw = iw >= 44 ? 10 : 8;
    for (int k = 0; k < nn && r < ih; k++, r++) {
        netif_t *f = &s->net.i[idx[k]];
        int xx = ix, xm = ix + iw;
        put(iy + r, xx, nw, f->up ? A_BOLD : A_GRAY, "%s", f->name);
        xx += nw;
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "%s ", U ? "↓" : "in");
        xx += put(iy + r, xx, xm - xx, f->rx_bps > 0 ? CP(C_IN) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  %s ", U ? "↑" : "out");
        xx += put(iy + r, xx, xm - xx, f->tx_bps > 0 ? CP(C_OUT) | A_BOLD : A_GRAY, "%7s", b);
        if (f->err_rate + f->drop_rate > 0 && xm - xx >= 12) /* only when it happens */
            xx += put(iy + r, xx, xm - xx, CP(C_CRIT) | A_BOLD, "  %.0f %s/s", f->err_rate + f->drop_rate,
                      f->err_rate > 0 ? "err" : "drop");
        if (f->quality >= 0 && xm - xx >= 10) {
            double d = f->signal_dbm;
            xx += put(iy + r, xx, xm - xx, d >= -67 ? CP(C_OK) : d >= -75 ? CP(C_WARN) : CP(C_CRIT), "  %.0f dBm", d);
        }
        if (f->wifi.ok && xm - xx >= 9) { /* the band, channel and link speed, as far as they fit */
            const wifi_t *wf = &f->wifi;
            char wb[48];
            if (wf->tx_mbps > 0 && xm - xx >= 24)
                sfmt(wb, sizeof wb, "  %s ch %d  %.0fM", wifi_band(wf->freq), wf->channel, wf->tx_mbps);
            else if (wf->tx_mbps > 0 && xm - xx >= 15)
                sfmt(wb, sizeof wb, "  %s %.0fM", wifi_band(wf->freq), wf->tx_mbps);
            else
                sfmt(wb, sizeof wb, "  %s", wifi_band(wf->freq));
            xx += put(iy + r, xx, xm - xx, wf->tx_mbps > 0 && wf->tx_mbps < 30 ? CP(C_WARN) : A_NORMAL, "%s", wb);
        }
        if (f->addr[0] && xm - xx >= (int)strlen(f->addr) + 2)
            put(iy + r, xx, xm - xx, A_GRAY, "  %s", f->addr);
        else if (f->speed > 0 && xm - xx >= 10)
            put(iy + r, xx, xm - xx, A_GRAY, "  %dM", f->speed);
    }
    if (s->net.q.on && r < ih)
        net_ping_row(&s->net.q, iy + r++, ix, iw);
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_rx, &s->h_tx, CP(C_IN), CP(C_OUT), U ? "↓" : "v", U ? "↑" : "^");
}

/* ========================================================= Temperatures */

typedef struct {
    char name[40];
    double v, crit;
    int mine; /* crit is your own limit */
} tent_t;


/* the same name twice in one panel (two chips that call themselves alike):
 * number the later ones */
static void unique_names(char (*nm)[40], int n)
{
    for (int i = 1; i < n; i++) {
        int same = 1;
        for (int j = 0; j < i; j++)
            same += !strcmp(nm[j], nm[i]);
        if (same > 1) {
            char b[40];
            sfmt(b, sizeof b, "%.33s #%d", nm[i], same);
            str_copy(nm[i], 40, b);
        }
    }
}

static int temps_list(sys_t *s, tent_t *T, int max)
{
    sensors_t *S = &s->sens;
    int n = 0;
    if (S->cpu_idx >= 0 && S->s[S->cpu_idx].ok && !S->s[S->cpu_idx].hidden && n < max) { /* CPU first */
        const sensor_t *t = &S->s[S->cpu_idx];
        str_copy(T[n].name, sizeof T[n].name, t->name[0] ? t->name : "CPU");
        T[n].v = t->value;
        T[n].crit = t->uhi > 0 ? t->uhi : t->crit;
        T[n].mine = t->uhi > 0;
        n++;
    }
    /* then GPUs with no hwmon chip of their own (NVIDIA, read through NVML) */
    for (int i = 0; i < s->gpus.n && n < max; i++) {
        const gpu_t *g = &s->gpus.g[i];
        if (g->temp_c < 0 || g->hwmon[0])
            continue;
        if (s->gpus.n > 1)
            sfmt(T[n].name, sizeof T[n].name, "GPU%d %s", i, g->vendor);
        else
            str_copy(T[n].name, sizeof T[n].name, "GPU");
        T[n].v = g->temp_c;
        T[n].crit = g->temp_lim > 0 ? g->temp_lim : 0; /* where it starts slowing itself down */
        T[n].mine = 0;
        n++;
    }
    for (int i = 0; i < S->n && n < max; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_TEMP || !t->ok || i == S->cpu_idx || t->hidden || !sensor_plausible(t))
            continue;
        sensor_short_name(S, i, T[n].name, sizeof T[n].name); /* "ACPI 2", "NVMe", "Core 3" */
        T[n].v = t->value;
        T[n].crit = t->uhi > 0 ? t->uhi : t->crit;
        T[n].mine = t->uhi > 0;
        n++;
    }
    char nm[64][40];
    for (int i = 0; i < n && i < 64; i++)
        str_copy(nm[i], sizeof nm[i], T[i].name);
    unique_names(nm, MIN(n, 64));
    for (int i = 0; i < n && i < 64; i++)
        str_copy(T[i].name, sizeof T[i].name, nm[i]);
    return n;
}

static int temps_cols(int iw)
{
    return MAX(1, iw / 30);
}

static int temps_need(sys_t *s, ui_state_t *u, int w)
{
    tent_t T[64];
    int n = temps_list(s, T, 64), c = temps_cols(w - 4);
    return n ? 2 + MIN((n + c - 1) / c, 8) : 3;
}

static void panel_temps(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    tent_t T[64];
    int n = temps_list(s, T, 64), hot = -1;
    for (int i = 0; i < n; i++)
        if (hot < 0 || T[i].v > T[hot].v)
            hot = i;
    char info[64] = "";
    if (hot >= 0)
        sfmt(info, sizeof info, "hottest: %s %.0f%s", T[hot].name, T[hot].v, DEG);
    box_(y, x, h, w, "Temperatures", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "No temperature sensors found in /sys/class/hwmon");
        return;
    }
    int cols = temps_cols(iw), rows = MIN((n + cols - 1) / cols, MIN(ih, 8)), cw = iw / cols;
    int shown = MIN(n, rows * cols);
    if (shown < n && rows <= 2) { /* short of room: side by side, the CPU first, as many as fit */
        int r = 0, kx = ix, km = ix + iw, i = 0;
        for (; i < n; i++) {
            char v[16];
            sfmt(v, sizeof v, "%.0f%s", T[i].v, DEG);
            int need = utf8_cols(T[i].name) + 1 + utf8_cols(v);
            if (kx > ix && kx + need > km) {
                if (r + 1 >= rows)
                    break;
                r++;
                kx = ix;
            }
            kv(iy + r, &kx, km, temp_attr_lim(T[i].v, T[i].mine ? T[i].crit : 0) | A_BOLD, T[i].name, "%s", v);
        }
        if (i < n)
            kv(iy + r, &kx, km, A_GRAY, "", "+%d more", n - i);
        return;
    }
    for (int i = 0; i < shown; i++) {
        int cy = iy + i % rows, cx = ix + (i / rows) * cw, aw = cw - (cols > 1 ? 3 : 0);
        int nw = MIN(14, aw / 2), vw = 5, bw = aw - nw - vw - 1;
        if (i == shown - 1 && shown < n) {
            put(cy, cx, aw, A_GRAY, "+%d more (Hardware view)", n - shown + 1);
            break;
        }
        double lim = T[i].mine ? T[i].crit : 0;
        put(cy, cx, nw, A_NORMAL, "%s", T[i].name);
        if (bw > 2)
            bar(cy, cx + nw, bw, 100.0 * T[i].v / (T[i].crit > 0 ? T[i].crit : 100.0),
                gradient_on() ? 0 : bar_temp_attr_lim(T[i].v, lim));
        char v[16];
        sfmt(v, sizeof v, "%.0f%s", T[i].v, DEG);
        put(cy, cx + aw - utf8_cols(v), vw, temp_attr_lim(T[i].v, lim) | A_BOLD, "%s", v);
    }
    if (ih > rows && s->cpu.temp_c >= 0) /* CPU temperature history */
        graph(iy + rows, ix, ih - rows, iw, &s->h_temp, 100, 0, 0);
}

/* ================================================================= Fans */

typedef struct {
    char name[40];
    double v, ref; /* RPM (or % when ref == 100 and pct set) */
    int pct;
    double min;    /* your minimum RPM, 0 = none */
} fent_t;

static int fans_list(sys_t *s, fent_t *F, int max)
{
    sensors_t *S = &s->sens;
    int n = 0;
    for (int i = 0; i < S->n && n < max; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_FAN || !t->ok || t->hidden)
            continue;
        sensor_short_name(S, i, F[n].name, sizeof F[n].name); /* "asus cpu_fan", "nct6798 fan2" */
        F[n].min = t->ulo;
        F[n].v = t->value;
        /* bar scale: the fan's rated max if the driver exposes it,
         * otherwise the fastest it has spun since start (min 2500 RPM) */
        F[n].ref = t->fmax > 0 ? t->fmax : MAX(t->peak, 2500.0);
        F[n].pct = 0;
        n++;
    }
    for (int i = 0; i < s->gpus.n && n < max; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (!g->nvml || (g->fan_rpm < 0 && g->fan_pct < 0))
            continue; /* sysfs GPUs' fans already come from their hwmon chip */
        sfmt(F[n].name, sizeof F[n].name, "GPU%d %s", i, g->vendor);
        if (g->fan_pct >= 0) {
            F[n].v = g->fan_pct;
            F[n].ref = 100;
            F[n].pct = 1;
        } else {
            F[n].v = g->fan_rpm;
            F[n].ref = 3500;
            F[n].pct = 0;
        }
        F[n].min = 0;
        n++;
    }
    return n;
}

static int fans_need(sys_t *s, ui_state_t *u, int w)
{
    fent_t F[32];
    int n = fans_list(s, F, 32);
    return 2 + (n ? MIN(n, 8) : 2);
}

static void panel_fans(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    fent_t F[32];
    int n = fans_list(s, F, 32);
    char info[32] = "";
    if (n)
        sfmt(info, sizeof info, "%d fan%s", n, n == 1 ? "" : "s");
    box_(y, x, h, w, "Fans", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "No fan speed sensors found.");
        if (ih > 1 && machine_desktop(&s->mach)) /* a board's fan chip may want its driver loaded */
            put(iy + 1, ix, iw, A_GRAY, "Desktop boards often need a driver: try 'sudo sensors-detect'");
        return;
    }
    if (n > ih) { /* more fans than rows: side by side, as many as fit */
        int r = 0, kx = ix, km = ix + iw, i = 0;
        for (; i < n; i++) {
            char v[24];
            if (F[i].pct)
                sfmt(v, sizeof v, "%.0f%%", F[i].v);
            else
                sfmt(v, sizeof v, "%.0f RPM", F[i].v);
            int need = utf8_cols(F[i].name) + 1 + utf8_cols(v);
            if (kx > ix && kx + need > km) {
                if (r + 1 >= ih)
                    break;
                r++;
                kx = ix;
            }
            int slow = F[i].min > 0 && F[i].v < F[i].min;
            kv(iy + r, &kx, km, slow ? CP(C_CRIT) | A_BOLD : F[i].v <= 0 ? A_GRAY : A_BOLD, F[i].name, "%s", v);
        }
        if (i < n) /* the title has the count; a note when there's room */
            kv(iy + r, &kx, km, A_GRAY, "", "+%d more", n - i);
        return;
    }
    int nw = MIN(18, iw / 3);
    for (int i = 0; i < n; i++) {
        char v[24];
        if (F[i].pct)
            sfmt(v, sizeof v, "%.0f%%", F[i].v);
        else
            sfmt(v, sizeof v, "%.0f RPM", F[i].v);
        int slow = F[i].min > 0 && F[i].v < F[i].min; /* slower than you said it should turn */
        put(iy + i, ix, nw, slow ? CP(C_CRIT) | A_BOLD : A_NORMAL, "%s", F[i].name);
        meter(iy + i, ix + nw, iw - nw, "", 0, 100.0 * F[i].v / F[i].ref, v, 9,
              slow ? CP(C_CRIT) : F[i].v <= 0 ? A_GRAY : gradient_on() ? 0 : CP(C_IN));
    }
}

/* ========================================================== Filesystems */

static int fs_need(sys_t *s, ui_state_t *u, int w)
{
    return 2 + MAX(MIN(s->disks.nfs, 6), 1);
}

static void panel_fs(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    disks_t *D = &s->disks;
    char info[32];
    sfmt(info, sizeof info, "%d mounted", D->nfs);
    box_(y, x, h, w, "Filesystems", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!D->nfs)
        put(iy, ix, iw, A_GRAY, "No local filesystems found");
    int nw = MIN(16, iw / 3);
    for (int i = 0; i < D->nfs && i < ih; i++) {
        fs_t *f = &D->fs[i];
        char a[16], b[16], v[40];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        sfmt(v, sizeof v, "%s/%s", a, b);
        put(iy + i, ix, nw - 1, A_BOLD, "%s", f->mount);
        meter(iy + i, ix + nw, iw - nw, "", 0, f->total ? 100.0 * (double)f->used / (double)f->total : 0, v, 11, 0);
    }
}

/* =============================================================== Health */

static double psi_tone(double v) /* stalls: 10% is noticeable, 25% is bad */
{
    return v >= 25 ? 100 : v >= 10 ? 70 : 0;
}

static int health_events(sys_t *s, const event_t **E, int max)
{
    int n = 0;
    for (int i = 0; n < max && i < s->health.nev; i++) {
        const event_t *e = health_event_at(&s->health, i);
        if (e->level != EV_INFO)
            E[n++] = e;
    }
    return n;
}

void event_line(int y, int x, int w, const event_t *e)
{
    char ts[24];
    time_t t = (time_t)e->t, now = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, now - t < 18 * 3600 ? "%H:%M:%S" : "%d %b %H:%M", &tm);
    int xx = x;
    xx += put(y, xx, w, A_GRAY, "%-12s", ts);
    attr_t ca = e->level == EV_ERR ? CP(C_CRIT) | A_BOLD : e->level == EV_WARN ? CP(C_WARN) : A_GRAY;
    xx += put(y, xx, x + w - xx, ca, "%-8s ", e->cat);
    put(y, xx, x + w - xx, A_NORMAL, "%s", e->text);
}

static int health_need(sys_t *s, ui_state_t *u, int w)
{
    const event_t *E[3];
    return 2 + 3 + MAX(1, health_events(s, E, 3));
}

static void panel_health(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    health_t *H = &s->health;
    char st[48], info[64];
    int lvl = health_alert(H, st, sizeof st);
    if (lvl)
        sfmt(info, sizeof info, "now: %s", st);
    else
        str_copy(info, sizeof info, "nothing wrong right now");
    box_(y, x, h, w, "Health", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    double now = (double)time(NULL);

    /* row 1: pressure (share of time tasks had to wait) */
    if (H->psi[0].ok) {
        static const char *const nm[3] = { "CPU", "Mem", "I/O" };
        int lw = iw >= 60 ? 9 : 0, cw = (iw - lw) / 3;
        if (lw)
            put(iy, ix, lw, A_GRAY, "Waiting");
        for (int i = 0; i < 3; i++) {
            double v = H->psi[i].some;
            char val[16];
            sfmt(val, sizeof val, v < 9.95 ? "%.1f%%" : "%.0f%%", v);
            meter(iy, ix + lw + i * cw, cw - (i < 2 ? 2 : 0), nm[i], 4, v, val, 5, bar_attr(psi_tone(v)));
        }
    } else {
        int kx = ix;
        kv(iy, &kx, ix + iw, A_GRAY, "Waiting", "%s", "n/a (kernel without pressure stall info)");
    }
    if (++r >= ih)
        return;

    /* row 2: throttling, out-of-memory kills */
    int kx = ix, km = ix + iw;
    if (!H->thr_ok) {
        kv(iy + r, &kx, km, A_GRAY, "Throttling", "%s", "n/a");
    } else {
        unsigned long long all = H->thr_count + H->core_count, since = all - H->thr_start;
        if (H->thr_last > 0 && now - H->thr_last < 10 && H->thr_pct >= 1)
            kv(iy + r, &kx, km, CP(C_CRIT) | A_BOLD, "Throttling", "NOW (%.0f%% of the time)", H->thr_pct);
        else if (H->thr_last > 0 && now - H->thr_last < 10)
            kv(iy + r, &kx, km, CP(C_CRIT) | A_BOLD, "Throttling", "%s", "just now");
        else if (since)
            kv(iy + r, &kx, km, CP(C_WARN) | A_BOLD, "Throttling", "%llu%s since start", since, U ? "×" : "x");
        else
            kv(iy + r, &kx, km, CP(C_OK), "Throttling", "%s", "none");
        if (all && !since)
            kv(iy + r, &kx, km, A_GRAY, "", "(%llu since boot)", all);
    }
    /* the CPU's power limit, once it has held the CPU back (needs root to know) */
    if (H->pl_now)
        kv(iy + r, &kx, km, A_BOLD, "Power limit", "holding the CPU back (%.0f W)",
           H->pl_now == PL_SHORT ? s->power.lim.eff_short_w : s->power.lim.eff_long_w);
    else if (H->pl_known && H->pl_held_s >= 5 && H->pl_busy_s > 0)
        kv(iy + r, &kx, km, A_NORMAL, "Power limit", "held the CPU back %.0f%% of its busy time",
           100.0 * H->pl_held_s / H->pl_busy_s);
    /* the GPU: held back by heat or a power limit (NVIDIA's own reasons) */
    for (int i = 0; i < s->gpus.n; i++) {
        const gpu_t *g = &s->gpus.g[i];
        if (!g->reasons_ok || g->busy_s <= 0)
            continue; /* never busy since start: nothing to say */
        char k[16];
        sfmt(k, sizeof k, s->gpus.n > 1 ? "GPU%d" : "GPU", i);
        double share = 100.0 * g->held_s / g->busy_s;
        if (g->slow_now)
            kv(iy + r, &kx, km, g->slow_now & GS_HEAT ? CP(C_WARN) | A_BOLD : A_BOLD, k, "slowing now (%s)",
               slow_words(g->slow_now));
        else if (g->held_s >= 5)
            kv(iy + r, &kx, km, g->heat_s >= g->pwr_s ? CP(C_WARN) : A_BOLD, k, "held back %.0f%% of its busy time", share);
        else
            kv(iy + r, &kx, km, CP(C_OK), k, "%s", "full speed");
        break;
    }
    if (s->mem.oom_ok) {
        unsigned long long since = s->mem.oom_kill - H->oom_start;
        if (H->oom_last > 0 && now - H->oom_last < 60)
            kv(iy + r, &kx, km, CP(C_CRIT) | A_BOLD, "OOM kills", "%s", "just now");
        else if (since)
            kv(iy + r, &kx, km, CP(C_WARN) | A_BOLD, "OOM kills", "%llu since start", since);
        else if (s->mem.oom_kill)
            kv(iy + r, &kx, km, A_BOLD, "OOM kills", "%llu since boot", s->mem.oom_kill);
        else
            kv(iy + r, &kx, km, CP(C_OK), "OOM kills", "%s", "none");
    }
    if (H->freezes) { /* only once it happened: the row is full enough */
        if (H->freeze_t > 0 && now - H->freeze_t < 60)
            kv(iy + r, &kx, km, CP(C_CRIT) | A_BOLD, "Froze", "%.1f s, just now", H->freeze_last);
        else
            kv(iy + r, &kx, km, CP(C_WARN) | A_BOLD, "Froze", "%d%s, longest %.1f s", H->freezes, U ? "×" : "x",
               H->freeze_max);
    }
    if (++r >= ih)
        return;

    /* row 3: PCIe errors, NVMe drive health, kernel log */
    kx = ix;
    if (!H->aer_ok)
        kv(iy + r, &kx, km, A_GRAY, "PCIe", "%s", "n/a");
    else if (H->pci_nonfatal + H->pci_fatal)
        kv(iy + r, &kx, km, CP(C_CRIT) | A_BOLD, "PCIe", "%llu uncorrected", H->pci_nonfatal + H->pci_fatal);
    else if (H->pci_cor)
        kv(iy + r, &kx, km, CP(C_WARN), "PCIe", "%llu corrected", H->pci_cor);
    else
        kv(iy + r, &kx, km, CP(C_OK), "PCIe", "%s", "no errors");
    if (H->nnvme || H->nsata) { /* NVMe and SATA drives together: the worst one, or "OK" */
        int ok = 0, read = 0, dl = 0, wear = -1, root = 0;
        char bad[48] = "";
        for (int i = 0; i < H->nnvme; i++) {
            nvme_t *v = &H->nvme[i];
            root |= v->state == -1;
            if (v->state != 1)
                continue;
            read++;
            if (v->crit || v->media_err) {
                dl = 2;
                sfmt(bad, sizeof bad, "%s %s", v->name, v->crit ? "WARNING" : "media errors");
            } else if (v->heat_open && dl < 1) { /* slowing itself down from heat now */
                dl = 1;
                sfmt(bad, sizeof bad, "%s slowed by heat", v->name);
            } else {
                ok++;
                wear = MAX(wear, v->used);
            }
        }
        for (int i = 0; i < H->nsata; i++) {
            sata_t *d = &H->sata[i];
            root |= d->state == -1;
            if (d->state != 1 && !(d->state == -3 && d->hours >= 0))
                continue;
            read++;
            if (d->failing || d->realloc > 0 || d->pending > 0 || d->uncorrect > 0) {
                dl = 2;
                sfmt(bad, sizeof bad, "%s %s", d->name, d->failing ? "FAILING" : "bad sectors");
            } else if (d->crc > 0 && dl < 2) {
                dl = 1;
                sfmt(bad, sizeof bad, "%s cable errors", d->name);
            } else {
                ok++;
                wear = MAX(wear, d->wear);
            }
        }
        if (dl)
            kv(iy + r, &kx, km, dl > 1 ? CP(C_CRIT) | A_BOLD : CP(C_WARN) | A_BOLD, "Drives", "%s", bad);
        else if (read == 1 && wear >= 0)
            kv(iy + r, &kx, km, CP(C_OK), "Drives", "OK, %d%% worn", wear);
        else if (read)
            kv(iy + r, &kx, km, CP(C_OK), "Drives", "%d OK%s", ok, root ? ", more with root" : "");
        else
            kv(iy + r, &kx, km, A_GRAY, "Drives", "%s", root ? "needs root" : "n/a");
    }
    for (int i = 0; i < s->usb.n && s->usb.nslow; i++) { /* a USB 3 drive held to USB 2 speed: slow copies */
        const usbdev_t *d = &s->usb.d[i];
        if (d->slow && (!strcmp(d->kind, "storage") || !strcmp(d->kind, "network") || !strcmp(d->kind, "Wi-Fi"))) {
            kv(iy + r, &kx, km, CP(C_WARN) | A_BOLD, "USB", "%s at USB 2 speed%s", d->blk[0] ? d->blk : d->name,
               s->usb.nslow > 1 ? " (and more)" : "");
            break;
        }
    }
    if (H->kmsg == 1) {
        if (H->kmsg_err)
            kv(iy + r, &kx, km, CP(C_WARN), "Kernel", "%u error%s since boot", H->kmsg_err, H->kmsg_err == 1 ? "" : "s");
        else
            kv(iy + r, &kx, km, CP(C_OK), "Kernel", "%s", "no errors");
    } else {
        kv(iy + r, &kx, km, A_GRAY, "Kernel log", "%s", H->kmsg == -1 ? "needs root" : "n/a");
    }
    for (int i = 0; i < s->power.nbat; i++) { /* a worn battery: there for good, so said once here */
        const battery_t *b = &s->power.bat[i];
        if (b->health > 0 && b->health < 80)
            kv(iy + r, &kx, km, b->health < 60 ? CP(C_CRIT) | A_BOLD : CP(C_WARN), "Battery", "worn, %.0f%% left", b->health);
    }
    r++;

    /* the rest: the latest problems, newest first */
    const event_t *E[64];
    int n = health_events(s, E, MIN(ih - r, 64));
    if (!n && r < ih)
        put(iy + r, ix, iw, A_GRAY, "%s",
            H->kmsg == 1 ? "No problems recorded since boot" : "No problems seen since hsm started");
    for (int i = 0; i < n && r < ih; i++, r++)
        event_line(iy + r, ix, iw, E[i]);
}

/* ============================================================= Voltages */

static int volt_list(sys_t *s, int *idx, int max)
{
    int n = 0;
    for (int i = 0; i < s->sens.n && n < max; i++)
        if (s->sens.s[i].type == SENS_VOLT && s->sens.s[i].ok && !s->sens.s[i].hidden)
            idx[n++] = i;
    return n;
}

static int volt_cols(int iw)
{
    return MAX(1, iw / 26);
}

static int volt_need(sys_t *s, ui_state_t *u, int w)
{
    int idx[64], n = volt_list(s, idx, 64), c = volt_cols(w - 4);
    return n ? 2 + MIN((n + c - 1) / c, 8) : 4;
}

static void panel_volt(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    int idx[64], n = volt_list(s, idx, 64);
    char info[32] = "";
    if (n)
        sfmt(info, sizeof info, "%d reading%s", n, n == 1 ? "" : "s");
    box_(y, x, h, w, "Voltages", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "No voltage sensors found.");
        if (ih > 1)
            put(iy + 1, ix, iw, A_GRAY, "Desktop boards report them once their sensor driver is loaded.");
        return;
    }
    int cols = volt_cols(iw), rows = MIN((n + cols - 1) / cols, ih), cw = iw / cols;
    for (int k = 0; k < n && k < rows * cols; k++) {
        sensor_t *t = &s->sens.s[idx[k]];
        int cy = iy + k % rows, cx = ix + (k / rows) * cw, aw = cw - (cols > 1 ? 3 : 0);
        int bad = sensor_bad(t);
        char nm[48], v[24];
        sensor_short_name(&s->sens, idx[k], nm, sizeof nm); /* "BAT1", "USB-C 1": not two "in0" */
        sfmt(v, sizeof v, "%.3f V", t->value);
        put(cy, cx, aw - 9, A_NORMAL, "%s", nm);
        put(cy, cx + aw - utf8_cols(v), utf8_cols(v), bad ? CP(C_CRIT) | A_BOLD : A_BOLD, "%s", v);
    }
}

/* ============================================================ Dashboard */

typedef struct {
    int (*need)(sys_t *, ui_state_t *, int w);                  /* rows incl. borders */
    void (*draw)(sys_t *, ui_state_t *, int y, int x, int h, int w);
    int weight; /* share of spare rows; spare rows become history graphs */
} pdef_t;

static const pdef_t PANELS[P_COUNT] = {
    [P_CPU] = { cpu_need, panel_cpu, 3 },       [P_GPU] = { gpu_need, panel_gpu, 1 },
    [P_MEM] = { mem_need, panel_mem, 1 },       [P_POWER] = { power_need, panel_power, 1 },
    [P_DISK] = { disk_need, panel_disk, 1 },    [P_NET] = { net_need, panel_net, 1 },
    [P_TEMPS] = { temps_need, panel_temps, 1 }, [P_FANS] = { fans_need, panel_fans, 0 },
    [P_FS] = { fs_need, panel_fs, 0 },          [P_HEALTH] = { health_need, panel_health, 1 },
    [P_SVC] = { svc_panel_need, svc_panel, 0 }, [P_VOLT] = { volt_need, panel_volt, 0 },
};

static int pweight(sys_t *s, int p)
{
    if (g_lay->graph_style == GRAPH_OFF)
        return 0;
    if (p == P_GPU) {
        for (int i = 0; i < s->gpus.n; i++)
            if (s->gpus.g[i].util >= 0)
                return 1;
        return 0;
    }
    if (p == P_TEMPS && s->cpu.temp_c < 0)
        return 0;
    return PANELS[p].weight;
}

/* left column width for a screen (or preview) W columns wide */
static int left_width(const layout_t *l, int W)
{
    int lw = l->left_pct ? W * l->left_pct / 100 : W >= 120 ? W * 55 / 100 : W / 2 + 2;
    return CLAMP(lw, MIN(30, W / 2), MAX(W - 30, W / 2));
}

/* Stack one column's panels into top_h rows: everyone gets what it needs,
 * spare rows go to panels with graphs (by weight); when short, panels
 * shrink from the bottom up and finally drop out. */
static void layout_column(sys_t *s, ui_state_t *u, const int *ids, int n, int y, int x, int w, int top_h)
{
    int h[P_COUNT], wt[P_COUNT], sum = 0, tw = 0;
    for (int i = 0; i < n; i++) {
        h[i] = PANELS[ids[i]].need(s, u, w);
        wt[i] = pweight(s, ids[i]);
        sum += h[i];
        tw += wt[i];
    }
    if (sum < top_h) {
        int extra = top_h - sum;
        if (!tw)
            h[n - 1] += extra;
        while (tw && extra > 0)
            for (int i = 0; i < n && extra > 0; i++)
                for (int k = 0; k < wt[i] && extra > 0; k++, extra--)
                    h[i]++;
        sum = top_h;
    }
    while (sum > top_h) { /* too tall: the tallest panel gives a row first (the upper one of two), so a short list stays whole */
        int done = 0, t = -1;
        for (int i = 0; i < n; i++)
            if (h[i] > 3 && (t < 0 || h[i] > h[t]))
                t = i;
        if (t >= 0) {
            h[t]--;
            sum--;
            done = 1;
        }
        if (!done)
            for (int i = n - 1; i >= 0; i--)
                if (h[i] > 0) {
                    sum -= h[i];
                    h[i] = 0;
                    break;
                }
    }
    if (sum < top_h)
        for (int i = n - 1; i >= 0; i--)
            if (h[i] > 0) {
                h[i] += top_h - sum;
                break;
            }
    for (int i = 0, yy = y; i < n; yy += h[i], i++)
        if (h[i] >= 3)
            PANELS[ids[i]].draw(s, u, yy, x, h[i], w);
}

static int visible(const layout_t *l, int c, int *ids)
{
    int n = 0;
    for (int i = 0; i < l->ncol[c]; i++)
        if (l->on[l->col[c][i]])
            ids[n++] = l->col[c][i];
    return n;
}

void dashboard(sys_t *s, ui_state_t *u)
{
    const layout_t *l = &u->lay;
    int top = 1, avail = LINES - 2, W = COLS;
    int ids[2][P_COUNT], n[2] = { visible(l, 0, ids[0]), visible(l, 1, ids[1]) };
    int both = n[0] && n[1];
    int lw = both ? left_width(l, W) : W;
    int xs[2] = { 0, both ? lw : 0 }, ws[2] = { lw, both ? W - lw : W };

    if (!n[0] && !n[1]) {
        if (l->show_procs) {
            draw_procs(s, u, top, 0, avail, W, 0);
        } else {
            box_(top, 0, avail, W, "Dashboard", NULL);
            put(top + 2, 3, W - 6, A_BOLD, "All dashboard panels are hidden.");
            put(top + 3, 3, W - 6, A_GRAY, "Press s to open Setup and turn some on.");
        }
        return;
    }

    /* the process list keeps at least ~35% of the screen */
    int proc_min = l->show_procs ? MAX(avail >= 34 ? 10 : avail >= 24 ? 7 : 5, avail * 35 / 100) : 0;
    int top_max = avail - proc_min;
    g_core_max = CLAMP(top_max / 3, 2, 12);

    int top_h = avail;
    if (l->show_procs) {
        int want = 0;
        for (int c = 0; c < 2; c++) {
            int sum = 0, wt = 0;
            for (int i = 0; i < n[c]; i++) {
                sum += PANELS[ids[c][i]].need(s, u, ws[c]);
                wt += pweight(s, ids[c][i]);
            }
            want = MAX(want, sum + (wt ? 6 : 0));
        }
        top_h = MIN(want, top_max);
        if (top_h < 8)
            top_h = MIN(8, avail);
    }
    for (int c = 0; c < 2; c++)
        if (n[c])
            layout_column(s, u, ids[c], n[c], top, xs[c], ws[c], top_h);
    if (l->show_procs)
        draw_procs(s, u, top + top_h, 0, avail - top_h, W, 0);
}

/* rows the preview needs to show every panel: one each, then a gap and the process list */
int dashboard_preview_rows(const layout_t *l)
{
    int ids[P_COUNT], rows = MAX(MAX(visible(l, 0, ids), visible(l, 1, ids)), 1);
    return rows + (l->show_procs ? 3 : 0);
}

/* Miniature of the dashboard for the Setup screen; panel hl is highlighted.
 * Every row belongs to one thing: when space is short the gaps go first, then
 * the process list gets a single row, and a column that still doesn't fit
 * ends in "+N more" instead of running under the process list. */
void dashboard_preview(const layout_t *l, int y, int x, int h, int w, int hl)
{
    int ids[2][P_COUNT], n[2] = { visible(l, 0, ids[0]), visible(l, 1, ids[1]) };
    int both = n[0] && n[1], rows = MAX(MAX(n[0], n[1]), 1);
    int bar = l->show_procs ? 2 : 0, gap = l->show_procs ? 1 : 0;
    if (rows + bar + gap > h)
        gap = 0;
    if (bar && rows + bar > h)
        bar = 1;
    int room = MAX(h - bar - gap, 1);                               /* rows for the panels */
    int per = CLAMP(room / rows, 1, 4), sh = per > 1 ? per - 1 : 1; /* block height */
    int lw = both ? left_width(l, w) : w;
    int xs[2] = { x, both ? x + lw : x }, ws[2] = { lw, both ? w - lw : w };
    if (!n[0] && !n[1])
        put(y, x, w, A_GRAY, "(no panels shown)");
    for (int c = 0; c < 2; c++) {
        int fit = n[c] <= room ? n[c] : room - 1; /* the last row then says how many more */
        for (int i = 0; i < fit; i++) {
            int p = ids[c][i];
            attr_t a = p == hl ? CP(C_THEAD) | A_BOLD : CP(C_GRAY) | A_REVERSE;
            for (int r = 0; r < sh; r++)
                fill(y + i * per + r, xs[c], ws[c] - 1, a);
            put(y + i * per, xs[c] + 1, ws[c] - 2, a, "%s", PANEL_NAME[p]);
        }
        if (fit < n[c]) { /* per is 1 here */
            int sel = 0;
            for (int i = fit; i < n[c]; i++)
                sel |= ids[c][i] == hl;
            attr_t a = sel ? CP(C_THEAD) | A_BOLD : CP(C_GRAY) | A_REVERSE;
            fill(y + fit, xs[c], ws[c] - 1, a);
            put(y + fit, xs[c] + 1, ws[c] - 2, a, "+%d more", n[c] - fit);
        }
    }
    if (bar) {
        int used = rows > room ? room : per > 1 ? rows * per - 1 : rows;
        int py = y + used + (used + 1 + bar <= h ? 1 : 0);
        for (int r = 0; r < bar; r++)
            fill(py + r, x, w, CP(C_SEL));
        put(py, x + 1, w - 2, CP(C_SEL), "Processes");
    }
}

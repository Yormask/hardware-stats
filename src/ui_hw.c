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
/* ui_hw.c - Hardware view: every sensor, GPU detail, drive, filesystem and interface */
#define _GNU_SOURCE
#include "ui_int.h"
#include "watch.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ============================================================= Hardware */

typedef struct {
    int y0, x, w, h, line, scroll;
} lc_t;

static int lc_row(lc_t *c)
{
    int r = c->line - c->scroll;
    c->line++;
    return (r >= 0 && r < c->h) ? c->y0 + r : -1;
}

static void lc_text(lc_t *c, attr_t a, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void lc_text(lc_t *c, attr_t a, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(y, c->x, c->w, a, "%s", b);
}

static void lc_title(lc_t *c, const char *t)
{
    if (c->line)
        lc_row(c);
    int y = lc_row(c);
    if (y >= 0) {
        int n = put(y, c->x, c->w, CP(C_TITLE) | A_BOLD, "%s ", t);
        attrset(CP(C_BORDER));
        for (int i = n; i < c->w; i++)
            mvaddstr(y, c->x + i, U ? "─" : "-");
        attrset(A_NORMAL);
    }
}

static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    int kw = MIN(16, c->w / 3);
    put(y, c->x, kw, A_GRAY, "%s", k);
    put(y, c->x + kw, c->w - kw, va, "%s", b);
}

static void hw_sensors(sys_t *s, lc_t *c)
{
    sensors_t *S = &s->sens;
    lc_title(c, "Sensors");
    if (!S->n) {
        lc_text(c, A_GRAY, "No hwmon sensors found in /sys/class/hwmon");
        return;
    }
    int last = -1, hidden = 0;
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (t->hidden) {
            hidden++;
            continue;
        }
        if (t->chip_idx != last) {
            lc_text(c, CP(C_INFO) | A_BOLD, "%s", t->chip);
            last = t->chip_idx;
        }
        int y = lc_row(c);
        if (y < 0)
            continue;
        int lw = MIN(22, c->w / 2), xx = c->x + 2 + lw, bad = sensor_bad(t);
        put(y, c->x + 2, lw, bad ? CP(C_CRIT) | A_BOLD : A_NORMAL, "%s", sensor_label(t));
        if (!t->ok) {
            put(y, xx, c->w - lw - 2, A_GRAY, "n/a");
            continue;
        }
        char lim[48] = "";
        if (!sensor_plausible(t)) { /* no chip gets there: nothing is connected to it */
            xx += put(y, xx, 9, A_GRAY, "%5.1f%s", t->value, DEG);
            put(y, xx + 1, c->x + c->w - xx - 1, A_GRAY, "%s", "nothing connected (no real reading)");
            continue;
        } else if (t->type == SENS_TEMP) {
            xx += put(y, xx, 9, temp_attr_lim(t->value, t->uhi) | A_BOLD, "%5.1f%s", t->value, DEG);
            double ref = t->uhi > 0 ? t->uhi : t->crit > 0 ? t->crit : 100.0;
            int bw = MIN(20, c->x + c->w - xx - 14);
            if (bw > 3) {
                bar(y, xx + 1, bw, 100.0 * t->value / ref, gradient_on() ? 0 : bar_temp_attr_lim(t->value, t->uhi));
                xx += bw + 1;
            }
            if (t->uhi > 0)
                sfmt(lim, sizeof lim, "your limit %.0f%s", t->uhi, DEG);
            else if (t->crit > 0)
                sfmt(lim, sizeof lim, "crit %.0f%s", t->crit, DEG);
        } else if (t->type == SENS_FAN) {
            xx += put(y, xx, 9, bad ? CP(C_CRIT) | A_BOLD : A_BOLD, "%5.0f RPM", t->value);
            if (t->ulo > 0)
                sfmt(lim, sizeof lim, "your minimum %.0f RPM", t->ulo);
        } else if (t->type == SENS_VOLT) {
            xx += put(y, xx, 10, bad ? CP(C_CRIT) | A_BOLD : A_BOLD, "%7.3f V", t->value);
            if (t->ulo > 0 && t->uhi > 0)
                sfmt(lim, sizeof lim, "your range %.2f - %.2f V", t->ulo, t->uhi);
            else if (t->ulo > 0 || t->uhi > 0)
                sfmt(lim, sizeof lim, "your %s %.2f V", t->ulo > 0 ? "minimum" : "maximum", t->ulo > 0 ? t->ulo : t->uhi);
            else if (t->lo > 0 || t->hi > 0)
                sfmt(lim, sizeof lim, "range %.2f - %.2f V", t->lo, t->hi);
        } else {
            xx += put(y, xx, 9, bad ? CP(C_CRIT) | A_BOLD : CP(C_WARN) | A_BOLD, "%6.2f W", t->value);
            if (t->uhi > 0)
                sfmt(lim, sizeof lim, "your limit %.0f W", t->uhi);
        }
        if (t->still) /* a placeholder, or nothing connected */
            str_copy(lim, sizeof lim, t->type == SENS_VOLT ? "0 V all along: nothing connected?" : "hasn't changed since hsm started");
        if (lim[0] && c->x + c->w - xx > 6)
            put(y, xx + 2, c->x + c->w - xx - 2, A_GRAY, "%s", lim);
    }
    if (hidden)
        lc_text(c, A_GRAY, "  %d hidden (Setup > Sensor names and limits)", hidden);
}

static void hw_health(sys_t *s, lc_t *c)
{
    health_t *H = &s->health;
    char a[24], b[24];
    lc_title(c, "Health");

    /* pressure stall information */
    if (H->psi[0].ok) {
        lc_text(c, A_GRAY, "%-16s %6s %6s %6s %6s   %-21s", "Waiting (PSI)", "now", "10s", "1min", "5min", "all tasks at once");
        static const char *const nm[3] = { "  CPU", "  Memory", "  Disk I/O" };
        for (int i = 0; i < 3; i++) {
            psi_t *p = &H->psi[i];
            char full[64] = "";
            if (i && p->has_full)
                sfmt(full, sizeof full, "%5.1f%% %5.1f%% %5.1f%%", p->full, p->favg10, p->favg60);
            int y = lc_row(c);
            if (y < 0)
                continue;
            int xx = c->x;
            xx += put(y, xx, 16, CP(C_INFO), "%-16s ", nm[i]);
            xx += put(y, xx, c->x + c->w - xx, pct_attr(p->some >= 25 ? 100 : p->some >= 10 ? 70 : 0) | A_BOLD, "%5.1f%% ", p->some);
            xx += put(y, xx, c->x + c->w - xx, A_NORMAL, "%5.1f%% %5.1f%% %5.1f%%   ", p->avg10, p->avg60, p->avg300);
            put(y, xx, c->x + c->w - xx, A_GRAY, "%s", full);
        }
        lc_text(c, A_GRAY, "  share of time tasks had to wait; 10%% or more is noticeable");
    } else {
        lc_kv(c, "Waiting (PSI)", A_GRAY, "%s", "not available: needs Linux 4.20+ with PSI enabled");
    }

    /* thermal throttling */
    if (H->thr_ok) {
        unsigned long long all = H->thr_count + H->core_count, since = all - H->thr_start;
        double now = (double)time(NULL);
        attr_t at = H->thr_last > 0 && now - H->thr_last < 10 ? CP(C_CRIT) | A_BOLD : since ? CP(C_WARN) | A_BOLD : A_BOLD;
        lc_kv(c, "Throttling", at, "%llu since boot (%.1f s in total)%s", all, H->thr_ms / 1000.0,
              since ? "" : ", none since start");
        if (since && H->thr_last > 0 && now - H->thr_last < 10)
            lc_kv(c, "", at, "%llu since hsm started, NOW (%.0f%% of the time)", since, H->thr_pct);
        else if (since)
            lc_kv(c, "", at, "%llu since hsm started", since);
        for (int i = 0; i < H->nthr && H->nthr > 1; i++)
            lc_kv(c, "", A_GRAY, "package %d: %llu times, %.1f s", H->thr[i].pkg, H->thr[i].count, (double)H->thr[i].ms / 1000.0);
        lc_text(c, A_GRAY, "  the CPU slows itself down when it reaches its temperature limit");
    } else {
        lc_kv(c, "Throttling", A_GRAY, "%s", "not reported by this CPU (Intel only)");
    }

    if (s->mem.oom_ok) {
        unsigned long long since = s->mem.oom_kill - H->oom_start;
        lc_kv(c, "OOM kills", since ? CP(C_CRIT) | A_BOLD : s->mem.oom_kill ? CP(C_WARN) : A_BOLD,
              "%llu since boot, %llu since hsm started", s->mem.oom_kill, since);
    }

    /* the whole system standing still; sleeping */
    if (H->freezes) {
        double now = (double)time(NULL);
        lc_kv(c, "Freezes", H->freeze_t > 0 && now - H->freeze_t < 60 ? CP(C_CRIT) | A_BOLD : CP(C_WARN) | A_BOLD,
              "%d since hsm started, longest %.1f s, %.1f s in total", H->freezes, H->freeze_max, H->frozen_total);
    } else {
        lc_kv(c, "Freezes", A_BOLD, "%s", "none since hsm started");
    }
    lc_text(c, A_GRAY, "  the whole system stood still for 2 s or more; the events say why");
    if (H->suspends) {
        fmt_duration(H->asleep_total, a, sizeof a);
        time_t t = (time_t)H->resume_t;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(b, sizeof b, "%H:%M", &tm);
        lc_kv(c, "Suspended", A_NORMAL, "%d time%s, %s asleep in total, last woke at %s", H->suspends,
              H->suspends == 1 ? "" : "s", a, b);
    }
    if (H->fan_stops)
        lc_kv(c, "Fans", CP(C_CRIT) | A_BOLD, "%d stopped while hot since hsm started", H->fan_stops);
    if (H->link_drops)
        lc_kv(c, "Network links", CP(C_WARN) | A_BOLD, "lost %d time%s since hsm started", H->link_drops,
              H->link_drops == 1 ? "" : "s");
    if (H->aer_ok) {
        attr_t pa = H->pci_nonfatal + H->pci_fatal ? CP(C_CRIT) | A_BOLD : H->pci_cor ? CP(C_WARN) : A_BOLD;
        lc_kv(c, "PCIe errors", pa, "%llu corrected, %llu non-fatal, %llu fatal since boot", H->pci_cor,
              H->pci_nonfatal, H->pci_fatal);
        for (int i = 0; i < H->npci; i++)
            lc_kv(c, "", A_GRAY, "%s %s: %llu / %llu / %llu", H->pci[i].dev, H->pci[i].name, H->pci[i].cor,
                  H->pci[i].nonfatal, H->pci[i].fatal);
    } else {
        lc_kv(c, "PCIe errors", A_GRAY, "%s", "not reported (no AER on this machine)");
    }
    if (H->files_max) {
        fmt_si((double)H->files, a, sizeof a);
        lc_kv(c, "Open files", A_NORMAL, "%s handles in use (whole system)", a);
    }
    for (int i = 0; i < s->power.nbat; i++) {
        const battery_t *bt = &s->power.bat[i];
        if (bt->health > 0 && bt->health < 80)
            lc_kv(c, "Battery", bt->health < 60 ? CP(C_CRIT) | A_BOLD : CP(C_WARN) | A_BOLD,
                  "%s holds %.0f%% of what it did when new: worn%s", bt->name, bt->health,
                  bt->health < 60 ? ", it won't last long on battery" : "");
    }

    /* NVMe drive health */
    for (int i = 0; i < H->nnvme; i++) {
        nvme_t *v = &H->nvme[i];
        char k[32];
        sfmt(k, sizeof k, "Drive %s", v->name);
        if (v->state != 1) {
            lc_kv(c, k, A_GRAY, "%s: health %s", v->model[0] ? v->model : "NVMe",
                  v->state == -1 ? "needs root (sudo hsm)" : "not available");
            continue;
        }
        lc_kv(c, k, v->crit || v->media_err ? CP(C_CRIT) | A_BOLD : CP(C_OK) | A_BOLD, "%s: %s", v->model,
              v->crit ? "WARNING" : v->media_err ? "media errors" : "OK");
        if (v->crit)
            lc_kv(c, "", CP(C_CRIT) | A_BOLD, "%s%s%s%s", v->crit & 1 ? "spare low  " : "", v->crit & 2 ? "temperature  " : "",
                  v->crit & 4 ? "reliability degraded  " : "", v->crit & 8 ? "read-only  " : "");
        lc_kv(c, "", v->used >= 90 ? CP(C_WARN) : A_NORMAL, "%d%% of rated wear used, spare %d%% (warns below %d%%)",
              v->used, v->spare, v->spare_thr);
        fmt_bytes(v->written_b, a, sizeof a);
        fmt_bytes(v->read_b, b, sizeof b);
        lc_kv(c, "", A_NORMAL, "%llu h powered on, %llu power cycles", v->hours, v->cycles);
        lc_kv(c, "", A_NORMAL, "%s written, %s read, now %.0f%s", a, b, v->temp_c, DEG);
        lc_kv(c, "", v->media_err ? CP(C_CRIT) | A_BOLD : v->unsafe ? A_NORMAL : A_GRAY,
              "%llu unsafe shutdowns, %llu media errors, %llu logged errors", v->unsafe, v->media_err, v->err_log);
        if (v->warn_min || v->crit_min)
            lc_kv(c, "", CP(C_WARN), "%llu min above its warning temperature, %llu min above critical", v->warn_min,
                  v->crit_min);
        if (v->heat_n[0] || v->heat_n[1] || v->heat_s[0] || v->heat_s[1]) {
            char part[2][48], when[32] = "";
            for (int t = 0; t < 2; t++) {
                char d[24] = "";
                if (v->heat_s[t])
                    fmt_duration((double)v->heat_s[t], d, sizeof d);
                sfmt(part[t], sizeof part[t], "%llu %s%s%s%s", v->heat_n[t], t ? "hard" : "light", d[0] ? " (" : "", d,
                     d[0] ? ")" : "");
            }
            if (v->heat_open) {
                sfmt(when, sizeof when, "%s", "; slowing down now");
            } else if (v->heat_t > 0) {
                time_t t = (time_t)v->heat_t;
                struct tm tm;
                if (localtime_r(&t, &tm))
                    strftime(when, sizeof when, "; last at %H:%M", &tm);
            }
            lc_kv(c, "", v->heat_open ? CP(C_WARN) | A_BOLD : v->heat_n[1] ? CP(C_WARN) : A_NORMAL,
                  "Heat slowdowns since new: %s, %s%s", part[0], part[1], when);
        }
    }

    /* SATA SSDs and hard disks */
    for (int i = 0; i < H->nsata; i++) {
        sata_t *d = &H->sata[i];
        char k[32], sum[200];
        int lvl;
        sfmt(k, sizeof k, "Drive %s", d->name);
        sata_summary(d, sum, sizeof sum, &lvl, ", ", DEG);
        const char *what = d->rotational ? "hard disk" : "SSD";
        if (d->state != 1 && !(d->state == -3 && d->hours >= 0)) {
            lc_kv(c, k, A_GRAY, "%s (SATA %s): %s", d->model[0] ? d->model : "drive", what, sum);
            continue;
        }
        lc_kv(c, k, lvl > 1 ? CP(C_CRIT) | A_BOLD : lvl ? CP(C_WARN) | A_BOLD : CP(C_OK) | A_BOLD, "%s (SATA %s): %s",
              d->model, what, d->failing ? "FAILING" : lvl > 1 ? "bad sectors" : lvl ? "cable errors" : "OK");
        if (d->state == -3)
            lc_kv(c, "", A_GRAY, "%s", "asleep now; these are the figures from when it last spun");
        lc_kv(c, "", lvl > 1 ? CP(C_CRIT) : A_NORMAL, "%lld reallocated, %lld pending, %lld uncorrectable sectors",
              MAX(d->realloc, 0), MAX(d->pending, 0), MAX(d->uncorrect, 0));
        if (d->crc >= 0)
            lc_kv(c, "", d->crc > 0 ? CP(C_WARN) : A_GRAY, "%lld cable errors (UDMA CRC)%s", d->crc,
                  d->crc > 0 ? ": reseat or replace the SATA cable" : "");
        char line[160] = "";
        if (d->wear >= 0)
            app(line, sizeof line, "%d%% of rated wear used, ", d->wear);
        if (d->hours >= 0)
            app(line, sizeof line, "%lld h powered on, ", d->hours);
        if (d->cycles >= 0)
            app(line, sizeof line, "%lld power cycles, ", d->cycles);
        if (d->unsafe >= 0)
            app(line, sizeof line, "%lld power losses, ", d->unsafe);
        if (d->temp_c > 0)
            app(line, sizeof line, "now %d%s, ", d->temp_c, DEG);
        size_t ll = strlen(line);
        if (ll >= 2)
            line[ll - 2] = '\0';
        if (line[0])
            lc_kv(c, "", A_NORMAL, "%s", line);
    }

    /* how the last sessions ended, when not normally */
    char hist[5][160];
    int nh = session_history(hist, 5);
    if (nh) {
        lc_kv(c, "Ended badly", CP(C_WARN) | A_BOLD, "%s", hist[0]);
        for (int i = 1; i < nh; i++)
            lc_kv(c, "", A_GRAY, "%s", hist[i]);
    }

    /* kernel messages and other events */
    if (H->kmsg == 1)
        lc_kv(c, "Kernel log", H->kmsg_err ? CP(C_WARN) | A_BOLD : A_BOLD, "%u errors, %u warnings since boot",
              H->kmsg_err, H->kmsg_warn);
    else
        lc_kv(c, "Kernel log", A_GRAY, "%s", H->kmsg == -1 ? "needs root (sudo hsm)" : "not available");
    if (!H->nev) {
        lc_text(c, A_GRAY, "  no events");
        return;
    }
    lc_text(c, A_GRAY, "Events, newest first:");
    for (int i = 0; i < H->nev && i < 40; i++) {
        int y = lc_row(c);
        if (y >= 0)
            event_line(y, c->x + 2, c->w - 2, health_event_at(H, i));
    }
}

static void hw_gpus(sys_t *s, lc_t *c)
{
    lc_title(c, "Graphics");
    if (!s->gpus.n) {
        lc_text(c, A_GRAY, "No GPU found in /sys/class/drm");
        return;
    }
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        char b[256] = "";
        lc_text(c, A_BOLD, "%s", g->name);
        app(b, sizeof b, "%s", g->driver[0] ? g->driver : "?");
        if (g->pci[0])
            app(b, sizeof b, "%sPCI %s", SEP, g->pci);
        app(b, sizeof b, "%s%s%s", SEP, g->card, g->integrated ? " (integrated)" : "");
        lc_kv(c, "  Driver", A_NORMAL, "%s", b);
        for (int k = 0; k < g->nscr; k++) { /* the screens on its ports */
            const screen_t *sc = &g->scr[k];
            char what[96] = "";
            if (sc->builtin)
                str_copy(what, sizeof what, "the built-in screen");
            else
                str_copy(what, sizeof what, sc->monitor[0] ? sc->monitor : "a screen");
            if (sc->inch >= 7)
                app(what, sizeof what, ", %.0f\"", sc->inch);
            if (sc->w > 0)
                app(what, sizeof what, ", %dx%d", sc->w, sc->h);
            lc_kv(c, k ? "" : "  Screens", sc->on ? A_NORMAL : A_GRAY, "%s: %s%s", sc->conn, what,
                  sc->on ? "" : " (connected, not in use)");
        }
        if (g->nv_state != NV_NONE) { /* NVIDIA: asleep or awake, and what hsm does about it */
            char held[64], why[160] = "";
            gpu_screen_holds(g, held, sizeof held);
            if (held[0] && g->nv_state == NV_ON)
                sfmt(why, sizeof why, "awake: the screen on %s keeps it on", held);
            const char *ps = g->nv_state == NV_ASLEEP ? "asleep (powered off)"
                           : g->nv_state == NV_IDLE   ? "idle: hsm let go of it so it can power off"
                           : held[0]                  ? why
                           : !g->pm_auto              ? "awake (it can't power off: runtime power management is off)"
                           : g->nv_fails              ? "awake: something else keeps it on (a program using it?)"
                                                      : "awake (powers off by itself when idle; hsm lets it)";
            lc_kv(c, "  Power state", g->nv_state == NV_ASLEEP ? CP(C_OK) : held[0] && g->nv_state == NV_ON ? CP(C_WARN) : A_NORMAL,
                  "%s", ps);
        }
        if (g->nv_state == NV_ASLEEP || g->nv_state == NV_IDLE) {
            if (g->note[0])
                lc_kv(c, "  Note", CP(C_WARN), "%s", g->note);
            continue; /* nothing more to read without waking it */
        }
        if (g->util >= 0) {
            b[0] = '\0';
            for (int e = 0; e < g->neng; e++)
                app(b, sizeof b, "%s%s %.0f%%", e ? "  " : "   (", g->eng[e].name, g->eng[e].pct);
            if (g->neng)
                app(b, sizeof b, ")");
            lc_kv(c, "  Load", pct_attr(g->util) | A_BOLD, "%s%.1f%%%s", g->util_est ? "~" : "", g->util, b);
        } else {
            lc_kv(c, "  Load", CP(C_WARN), "%s", g->note[0] ? g->note : "n/a");
        }
        if (g->mem_total > 0) {
            char a1[16], a2[16];
            fmt_bytes(g->mem_used, a1, sizeof a1);
            fmt_bytes(g->mem_total, a2, sizeof a2);
            lc_kv(c, "  VRAM", A_BOLD, "%s / %s (%.0f%%)", a1, a2, 100.0 * g->mem_used / g->mem_total);
        }
        if (g->clk_mhz >= 0)
            lc_kv(c, "  Clock", A_BOLD, "%.0f MHz%s", g->clk_mhz, g->clk_max_mhz > 0 ? "" : "");
        if (g->clk_max_mhz > 0)
            lc_kv(c, "  Max clock", A_NORMAL, "%.0f MHz", g->clk_max_mhz);
        if (g->mem_clk_mhz > 0)
            lc_kv(c, "  Memory clock", A_NORMAL, "%.0f MHz", g->mem_clk_mhz);
        if (g->temp_c >= 0) {
            char lim[96] = "";
            if (g->temp_lim > 0)
                app(lim, sizeof lim, "  (slows down from %.0f%s", g->temp_lim, DEG);
            if (g->temp_slow > 0 && g->temp_slow != g->temp_lim)
                app(lim, sizeof lim, "%shardware slowdown %.0f%s", lim[0] ? "; " : "  (", g->temp_slow, DEG);
            if (g->temp_shut > 0)
                app(lim, sizeof lim, "%sshutdown %.0f%s", lim[0] ? ", " : "  (", g->temp_shut, DEG);
            if (lim[0])
                app(lim, sizeof lim, ")");
            lc_kv(c, "  Temperature", temp_attr_lim(g->temp_c, g->temp_lim) | A_BOLD, "%.1f%s%s", g->temp_c, DEG, lim);
        }
        if (g->reasons_ok) { /* why it isn't at full speed, as the driver says */
            char since[128] = "";
            if (g->busy_s > 0 && g->held_s > 0)
                sfmt(since, sizeof since, "; held back %.0f%% of its busy time since start (heat %.0f%%, power limit %.0f%%)",
                     100.0 * g->held_s / g->busy_s, 100.0 * g->heat_s / g->busy_s, 100.0 * g->pwr_s / g->busy_s);
            if (g->slow_now)
                lc_kv(c, "  Slowing down", g->slow_now & GS_HEAT ? CP(C_WARN) | A_BOLD : A_BOLD, "now: %s%s",
                      g->slow_now == (GS_HEAT | GS_POWER) ? "heat and its power limit" : g->slow_now & GS_HEAT ? "heat" : "its power limit",
                      since);
            else
                lc_kv(c, "  Slowing down", A_NORMAL, "no%s", since);
        }
        if (g->power_w >= 0) {
            if (g->power_cap_w > 0)
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W of %.0f W limit", g->power_w, g->power_cap_w);
            else
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W%s", g->power_w,
                      g->integrated && !strcmp(g->vendor, "Intel") ? "  (RAPL uncore)" : "");
        }
        if (g->fan_pct >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f%%", g->fan_pct);
        else if (g->fan_rpm >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f RPM", g->fan_rpm);
        if (g->enc_pct >= 0 || g->dec_pct >= 0)
            lc_kv(c, "  Video", A_BOLD, "encoding %.0f%%, decoding %.0f%%", MAX(g->enc_pct, 0), MAX(g->dec_pct, 0));
        if (g->pcie_gen > 0 && g->pcie_width > 0) {
            int below = (g->pcie_gen_max > g->pcie_gen) || (g->pcie_width_max > g->pcie_width);
            if (below)
                lc_kv(c, "  PCIe link", A_BOLD, "%d.0 x%d now, up to %d.0 x%d%s", g->pcie_gen, g->pcie_width,
                      MAX(g->pcie_gen_max, g->pcie_gen), MAX(g->pcie_width_max, g->pcie_width),
                      g->util < 30 ? "  (slower while idle, to save power)" : "");
            else
                lc_kv(c, "  PCIe link", A_BOLD, "%d.0 x%d (its best)", g->pcie_gen, g->pcie_width);
        }
        if (g->util_est)
            lc_kv(c, "  Note", A_GRAY, "%s", "load estimated from RC6 sleep time; run as root for exact per-engine load");
        else if (g->util >= 0 && g->note[0])
            lc_kv(c, "  Note", A_GRAY, "%s", g->note);
    }
}

static void hw_power(sys_t *s, lc_t *c)
{
    power_t *p = &s->power;
    lc_title(c, "Power");
    if (s->total_w >= 0)
        lc_kv(c, "Estimated draw", CP(C_WARN) | A_BOLD, "%.1f W  (%s)", s->total_w, s->total_w_src);
    else if (s->total_why[0])
        lc_kv(c, "Estimated draw", A_GRAY, "the CPU part %s: the GPUs alone are no total", s->total_why);
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        char k[48];
        sfmt(k, sizeof k, "%sRAPL %s", z->depth ? "  " : "", z->name);
        if (!z->readable)
            lc_kv(c, k, CP(C_WARN), "%s", "no permission");
        else if (z->watts >= 0 && p->psys_bad && !strcmp(z->name, "psys"))
            lc_kv(c, k, A_GRAY, "%.2f W  (less than the CPU inside it: not connected, left out)", z->watts);
        else if (z->watts >= 0)
            lc_kv(c, k, A_BOLD, "%.2f W", z->watts);
        else
            lc_kv(c, k, A_GRAY, "%s", "...");
    }
    if (p->rapl_denied && geteuid() != 0) {
        lc_text(c, CP(C_WARN), "RAPL energy counters are root-only since Linux 5.10.");
        lc_text(c, CP(C_WARN), "Run 'sudo hsm' to see CPU / DRAM / iGPU power.");
    }
    if (!p->nrapl)
        lc_text(c, A_GRAY, "No RAPL power domains (/sys/class/powercap)");
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        char tl[24] = "";
        if (b->time_left > 0)
            fmt_duration(b->time_left, tl, sizeof tl);
        lc_kv(c, b->name, A_BOLD, "%s, %.0f%%%s%s%s", b->status, b->capacity, b->watts >= 0 ? "" : "",
              tl[0] ? ", " : "", tl);
        if (b->watts >= 0)
            lc_kv(c, "  Rate", CP(C_WARN) | A_BOLD, "%.2f W", b->watts);
        if (b->health >= 0) {
            char wh[48] = "";
            if (b->full_wh > 0 && b->design_wh > 0)
                sfmt(wh, sizeof wh, ": %.1f of %.1f Wh", b->full_wh, b->design_wh);
            lc_kv(c, "  Health", b->health < 60 ? CP(C_CRIT) | A_BOLD : b->health < 80 ? CP(C_WARN) : A_BOLD,
                  "%.0f%% of its original capacity%s%s", b->health, wh, b->health < 80 ? " (worn)" : "");
        }
        if (b->cycles >= 0)
            lc_kv(c, "  Charge cycles", A_NORMAL, "%ld", b->cycles);
        if (b->lim_ok) {
            if (b->lim_end > 0 && b->lim_start > 0)
                lc_kv(c, "  Charge limit", A_BOLD, "stops at %d%%, starts again below %d%%", b->lim_end, b->lim_start);
            else if (b->lim_end > 0)
                lc_kv(c, "  Charge limit", A_BOLD, "stops charging at %d%%", b->lim_end);
            else
                lc_kv(c, "  Charge limit", A_NORMAL, "%s", "none: charges to 100%");
        }
    }
    if (p->ac_online >= 0)
        lc_kv(c, "AC adapter", A_BOLD, "%s", p->ac_online ? "online" : "offline");
}

/* "30 s", "2.4 ms", "4 min" */
static void fmt_window(double sec, char *out, size_t n)
{
    if (sec < 1)
        sfmt(out, n, "%.1f ms", sec * 1000);
    else if (sec < 120)
        sfmt(out, n, "%.0f s", sec);
    else
        sfmt(out, n, "%.0f min", sec / 60);
}

/* how much the CPU package may draw, and whether that holds it back */
static void hw_cpu_limits(sys_t *s, lc_t *c)
{
    const cpulim_t *L = &s->power.lim;
    const health_t *H = &s->health;
    if (!L->ok)
        return;
    char line[160] = "", win[24];
    if (L->eff_long_w > 0) {
        app(line, sizeof line, "%.0f W sustained", L->eff_long_w);
        if (L->long_s > 0) {
            fmt_window(L->long_s, win, sizeof win);
            app(line, sizeof line, " (averaged over %s)", win);
        }
    } else if (L->long_off) {
        app(line, sizeof line, "%s", "no sustained limit (switched off)");
    }
    if (L->eff_short_w > 0)
        app(line, sizeof line, "%s%.0f W in short boosts", line[0] ? ", " : "", L->eff_short_w);
    lc_kv(c, "Power limits", A_BOLD, "%s", line);
    /* the firmware's own copy, when it differs: the lower one is what counts */
    int dl = L->fw_long_w > 0 && L->long_w > 0 && L->fw_long_w != L->long_w;
    int ds = L->fw_short_w > 0 && L->short_w > 0 && L->fw_short_w != L->short_w;
    if (dl || ds)
        lc_kv(c, "", A_GRAY, "CPU %.0f / %.0f W, firmware %.0f / %.0f W: the lower one applies", L->long_w,
              L->short_w, L->fw_long_w, L->fw_short_w);
    if (H->pl_now)
        lc_kv(c, "", CP(C_WARN) | A_BOLD, "holding the CPU back now: the package draws %.1f W%s", s->power.pkg_w,
              H->pl_now == PL_SHORT ? " (the boost limit)" : "");
    else if (H->pl_known && H->pl_held_s >= 5 && H->pl_busy_s > 0)
        lc_kv(c, "", A_NORMAL, "held the CPU back %.0f%% of its busy time since start",
              100.0 * H->pl_held_s / H->pl_busy_s);
    else if (H->pl_known)
        lc_kv(c, "", A_GRAY, "%s", "haven't held the CPU back since start");
    else if (s->power.rapl_denied && geteuid() != 0)
        lc_kv(c, "", A_GRAY, "%s", "run 'sudo hsm' to see when they hold the CPU back");
}

static void hw_system(sys_t *s, lc_t *c)
{
    cpu_t *cv = &s->cpu;
    mem_t *m = &s->mem;
    char a[24], b[24], d[24];
    lc_title(c, "System");
    lc_kv(c, "Host", A_BOLD, "%s", s->host);
    const machine_t *mc = &s->mach;
    if (mc->ok) { /* as the firmware describes it */
        char mm[200] = "";
        app(mm, sizeof mm, "%s", mc->model[0] ? mc->model : "?");
        if (mc->type[0])
            app(mm, sizeof mm, " (%s)", mc->type);
        lc_kv(c, "Machine", A_BOLD, "%s", mm);
        mm[0] = '\0';
        if (mc->vendor[0])
            app(mm, sizeof mm, "made by %s", mc->vendor);
        if (mc->board[0])
            app(mm, sizeof mm, "%sboard %s", mm[0] ? SEP : "", mc->board);
        if (mm[0])
            lc_kv(c, "", A_NORMAL, "%s", mm);
        if (mc->bios_version[0]) {
            char bd[32] = "";
            if (mc->bios_date) {
                struct tm tm;
                memset(&tm, 0, sizeof tm);
                tm.tm_year = mc->bios_date / 10000 - 1900;
                tm.tm_mon = mc->bios_date / 100 % 100 - 1;
                tm.tm_mday = mc->bios_date % 100;
                tm.tm_hour = 12;
                mktime(&tm);
                strftime(bd, sizeof bd, " from %d %b %Y", &tm);
            }
            lc_kv(c, "BIOS", A_NORMAL, "%s%s%s%s%s", mc->bios_version, bd, mc->bios_vendor[0] ? " (" : "",
                  mc->bios_vendor, mc->bios_vendor[0] ? ")" : "");
        }
    }
    lc_kv(c, "OS", A_NORMAL, "%s", s->os);
    lc_kv(c, "Kernel", A_NORMAL, "%s (%s)", s->kernel, s->arch);
    fmt_duration(cv->uptime, a, sizeof a);
    lc_kv(c, "Uptime", A_NORMAL, "%s", a);
    lc_kv(c, "Load average", A_BOLD, "%.2f  %.2f  %.2f", cv->load[0], cv->load[1], cv->load[2]);
    lc_kv(c, "Tasks", A_NORMAL, "%d processes, %d threads, %d running, %d blocked", s->procs.n, s->procs.threads,
          s->procs.running, s->procs.diskwait);
    fmt_si(cv->ctxt_rate, a, sizeof a);
    fmt_si(cv->intr_rate, b, sizeof b);
    fmt_si(cv->fork_rate, d, sizeof d);
    lc_kv(c, "Activity", A_NORMAL, "%s ctx switches/s, %s interrupts/s, %s forks/s", a, b, d);

    lc_title(c, "CPU");
    lc_kv(c, "Model", A_BOLD, "%s", cv->model);
    lc_kv(c, "Topology", A_NORMAL, "%d cores, %d threads (%d online)", cv->cores, cv->threads, cv->ncpu);
    lc_kv(c, "Usage", A_NORMAL, "%.1f%%  (user %.1f%%, system %.1f%%, iowait %.1f%%, steal %.1f%%)", cv->total,
          cv->user, cv->sys, cv->iowait, cv->steal);
    if (cv->mhz_avg > 0)
        lc_kv(c, "Clock", A_NORMAL, "avg %.0f MHz, max %.0f MHz", cv->mhz_avg, cv->mhz_max);
    if (cv->temp_c >= 0)
        lc_kv(c, "Temperature", temp_attr(cv->temp_c) | A_BOLD, "%.1f%s", cv->temp_c, DEG);
    if (cv->driver[0] || cv->governor[0]) {
        char pw[256] = "";
        app(pw, sizeof pw, "%s", cv->driver[0] ? cv->driver : "?");
        if (cv->pstate[0])
            app(pw, sizeof pw, " (%s)", cv->pstate);
        if (cv->governor[0])
            app(pw, sizeof pw, "%sgovernor %s", SEP, cv->governor);
        lc_kv(c, "Power settings", A_NORMAL, "%s", pw);
        pw[0] = '\0';
        if (cv->epp[0])
            lc_kv(c, "", A_NORMAL, "energy preference %s", cv->epp);
        if (cv->turbo >= 0)
            app(pw, sizeof pw, "turbo %s", cv->turbo ? "on" : "OFF");
        if (cv->lim_max > 0)
            app(pw, sizeof pw, "%slimits %.0f-%.0f MHz", pw[0] ? SEP : "", cv->lim_min, cv->lim_max);
        if (cv->profile[0])
            app(pw, sizeof pw, "%sprofile %s", pw[0] ? SEP : "", cv->profile);
        int capped = cv->lim_max > 0 && cv->hw_max > 0 && cv->lim_max < cv->hw_max - 1;
        if (pw[0])
            lc_kv(c, "", capped || cv->turbo == 0 ? CP(C_WARN) : A_NORMAL, "%s%s", pw,
                  capped ? "  (below what the CPU can do)" : "");
    }
    hw_cpu_limits(s, c);
    if (cv->nidle) { /* "C1 2%  C6 12%  C10 55%", wrapped to the column */
        char id[256] = "", w[24], tok[48];
        int first = 1, vw = c->w - MIN(16, c->w / 3) - 1;
        for (int k = 0; k < cv->nidle; k++) {
            if (!strcmp(cv->idle[k].name, "POLL"))
                continue;
            sfmt(tok, sizeof tok, "%s %.0f%%%s  ", cv->idle[k].name, cv->idle[k].pct, cv->idle[k].disabled ? " (off)" : "");
            if (id[0] && utf8_cols(id) + utf8_cols(tok) > vw) {
                lc_kv(c, first ? "Idle states" : "", A_NORMAL, "%s", id);
                first = 0;
                id[0] = '\0';
            }
            app(id, sizeof id, "%s", tok);
        }
        if (id[0])
            lc_kv(c, first ? "Idle states" : "", A_NORMAL, "%s", id);
        fmt_si(cv->wakeups, w, sizeof w);
        lc_kv(c, "", A_GRAY, "%s wake-ups/s (deeper states save more power)", w);
    }
    if (cv->mhz_avg > 0) {
        int per = MAX(1, (c->w - 2) / 13);
        char line[512] = "";
        for (int i = 0; i < cv->ncpu; i++) {
            app(line, sizeof line, "cpu%-3d%5.0f  ", cv->id[i], cv->mhz[i]);
            if ((i + 1) % per == 0 || i == cv->ncpu - 1) {
                lc_text(c, A_GRAY, "  %s", line);
                line[0] = '\0';
            }
        }
    }

    lc_title(c, "Memory");
    fmt_bytes((double)m->total, a, sizeof a);
    lc_kv(c, "Total", A_BOLD, "%s", a);
    fmt_bytes((double)m->used, a, sizeof a);
    lc_kv(c, "Used", A_BOLD, "%s (%.1f%%)", a, m->total ? 100.0 * (double)m->used / (double)m->total : 0);
    fmt_bytes((double)m->avail, a, sizeof a);
    lc_kv(c, "Available", A_NORMAL, "%s", a);
    fmt_bytes((double)m->buffcache, a, sizeof a);
    fmt_bytes((double)m->shmem, b, sizeof b);
    lc_kv(c, "Buff/cache", A_NORMAL, "%s  (shared %s)", a, b);
    fmt_bytes((double)m->dirty, a, sizeof a);
    fmt_bytes((double)m->writeback, b, sizeof b);
    lc_kv(c, "Dirty/writeback", A_NORMAL, "%s / %s", a, b);
    fmt_bytes((double)m->swap_used, a, sizeof a);
    fmt_bytes((double)m->swap_total, b, sizeof b);
    lc_kv(c, "Swap", A_NORMAL, "%s / %s", a, b);
    fmt_rate(m->swapin_bps, a, sizeof a);
    fmt_rate(m->swapout_bps, b, sizeof b);
    lc_kv(c, "Swap I/O", A_NORMAL, "in %s, out %s, %.0f major faults/s", a, b, m->majflt_rate);
    for (int i = 0; i < m->nswap; i++) {
        const swapdev_t *sw = &m->swap[i];
        char more[96] = "";
        fmt_bytes((double)sw->used, a, sizeof a);
        fmt_bytes((double)sw->size, b, sizeof b);
        if (sw->zram) { /* how well it compresses, and what it costs in RAM */
            char r[24];
            if (sw->compr > 0 && sw->orig > 0)
                sfmt(more, sizeof more, ", %.1fx smaller%s%s%s", (double)sw->orig / (double)sw->compr,
                     sw->algo[0] ? " (" : "", sw->algo, sw->algo[0] ? ")" : "");
            if (sw->ram > 0) {
                fmt_bytes((double)sw->ram, r, sizeof r);
                app(more, sizeof more, ", takes %s", r);
            }
        } else {
            sfmt(more, sizeof more, ", priority %d", sw->prio);
        }
        lc_kv(c, i ? "" : "Swap devices", A_NORMAL, "%s %s: %s of %s%s", sw->name,
              sw->zram ? "compressed RAM" : sw->file ? "file" : "partition", a, b, more);
    }
    if (m->zswap == 1) {
        char st[64] = "";
        if (m->zswap_stored && m->zswap_pool) {
            fmt_bytes((double)m->zswap_stored, a, sizeof a);
            fmt_bytes((double)m->zswap_pool, b, sizeof b);
            sfmt(st, sizeof st, ": %s kept in %s of RAM", a, b);
        }
        lc_kv(c, "zswap", A_NORMAL, "on (a compressed cache in front of swap%s%s)%s", m->zswap_algo[0] ? ", " : "",
              m->zswap_algo, st);
    }
}

static void hw_storage(sys_t *s, lc_t *c)
{
    disks_t *D = &s->disks;
    char a[24], b[24], d[24];
    lc_title(c, "Drives");
    if (!D->n)
        lc_text(c, A_GRAY, "No physical drives found");
    for (int i = 0; i < D->n; i++) {
        disk_t *k = &D->d[i];
        fmt_bytes((double)k->size, a, sizeof a);
        lc_text(c, A_BOLD, "%-10s %-5s %6s  %s", k->name, k->kind, a, k->model);
        fmt_rate(k->rd_bps, a, sizeof a);
        fmt_rate(k->wr_bps, b, sizeof b);
        fmt_si(k->rd_iops + k->wr_iops, d, sizeof d);
        lc_text(c, A_NORMAL, "  now   read %s  write %s  %s IOPS  busy %.0f%%", a, b, d, k->util);
        double lat = MAX(k->rd_lat, k->wr_lat);
        lc_text(c, lat >= 200 ? CP(C_CRIT) | A_BOLD : lat >= 50 ? CP(C_WARN) : A_NORMAL,
                "  time per request   read %.1f ms  write %.1f ms  in flight %u", k->rd_lat, k->wr_lat, k->inflight);
        fmt_bytes((double)k->rd_total, a, sizeof a);
        fmt_bytes((double)k->wr_total, b, sizeof b);
        lc_text(c, A_GRAY, "  since boot   read %s  written %s", a, b);
    }
    lc_title(c, "Filesystems");
    if (!D->nfs)
        lc_text(c, A_GRAY, "No local filesystems found");
    for (int i = 0; i < D->nfs; i++) {
        fs_t *f = &D->fs[i];
        int y = lc_row(c);
        if (y < 0)
            continue;
        char v[64];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        sfmt(v, sizeof v, "%s/%s %-5s", a, b, f->type);
        double pct = f->total ? 100.0 * (double)f->used / (double)f->total : 0;
        int lw = MIN(18, c->w / 3);
        put(y, c->x, lw - 1, A_BOLD, "%s", f->mount);
        meter(y, c->x + lw, c->w - lw, "", 0, pct, v, 18, 0);
    }
}

static void hw_network(sys_t *s, lc_t *c)
{
    char a[24], b[24];
    lc_title(c, "Network");
    if (!s->net.n)
        lc_text(c, A_GRAY, "No interfaces (besides loopback)");
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        char sp[24] = "";
        if (f->speed > 0)
            sfmt(sp, sizeof sp, "%d Mb/s", f->speed);
        lc_text(c, f->up ? A_BOLD : A_GRAY, "%-12s %-8s %-8s %-11s %s", f->name,
                f->wireless ? "wifi" : f->is_virtual ? "virtual" : "ethernet", f->state, sp, f->addr);
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        char ta[24], tb[24];
        fmt_bytes((double)f->rx_total, ta, sizeof ta);
        fmt_bytes((double)f->tx_total, tb, sizeof tb);
        lc_text(c, A_GRAY, "  %s %s  %s %s   total rx %s  tx %s", U ? "↓" : "in", a, U ? "↑" : "out", b, ta, tb);
        attr_t sa = f->signal_dbm >= -67 ? CP(C_OK) : f->signal_dbm >= -75 ? CP(C_WARN) : CP(C_CRIT);
        if (f->wifi.ok) {
            const wifi_t *wf = &f->wifi;
            char gen[16] = "", wd[24] = "", sig[48] = "", tx[16] = "?", rx[16] = "?", rs[32] = "", ct[32] = "";
            if (wf->gen)
                sfmt(gen, sizeof gen, "Wi-Fi %d, ", wf->gen);
            if (wf->width)
                sfmt(wd, sizeof wd, ", %d MHz wide", wf->width);
            if (f->quality >= 0)
                sfmt(sig, sizeof sig, ", signal %.0f dBm (%d%%)", f->signal_dbm, f->quality);
            lc_text(c, f->quality >= 0 ? sa : A_NORMAL, "  %s%s channel %d%s%s", gen, wifi_band(wf->freq), wf->channel,
                    wd, sig);
            if (wf->tx_mbps > 0)
                sfmt(tx, sizeof tx, "%.0f", wf->tx_mbps);
            if (wf->rx_mbps > 0)
                sfmt(rx, sizeof rx, "%.0f", wf->rx_mbps);
            if (wf->retry_pct >= 0)
                sfmt(rs, sizeof rs, ", %.0f%% sent again", wf->retry_pct);
            if (wf->connected_s) {
                char d[24];
                fmt_duration((double)wf->connected_s, d, sizeof d);
                sfmt(ct, sizeof ct, ", connected %s", d);
            }
            lc_text(c, wf->retry_pct >= 15 || (wf->tx_mbps > 0 && wf->tx_mbps < 30) ? CP(C_WARN) : A_NORMAL,
                    "  link %s Mb/s out, %s Mb/s in%s%s", tx, rx, rs, ct);
        } else if (f->quality >= 0) {
            lc_text(c, sa, "  Wi-Fi signal %.0f dBm, link quality %d%%", f->signal_dbm, f->quality);
        }
        if (f->errors || f->drops || f->err_rate > 0 || f->drop_rate > 0)
            lc_text(c, f->err_rate + f->drop_rate > 0 ? CP(C_CRIT) | A_BOLD : A_GRAY,
                    "  %llu errors, %llu dropped packets since boot (now %.0f/s)", f->errors, f->drops,
                    f->err_rate + f->drop_rate);
    }
    const netq_t *q = &s->net.q;
    if (!q->on) {
        lc_kv(c, "Network check", A_GRAY, "%s", "off (Setup > Behaviour: pings for delay and lost packets)");
        return;
    }
    if (q->err) {
        lc_kv(c, "Network check", CP(C_WARN), "can't ping: %s", q->err == EACCES || q->err == EPERM
                                                ? "not allowed for this user (sysctl net.ipv4.ping_group_range)" : strerror(q->err));
        return;
    }
    lc_kv(c, "Network check", A_NORMAL, "%s", "a ping every 2 s; figures over the last minute");
    static const char *const nm[NQ_COUNT] = { "  Router", "  Internet" };
    for (int k = 0; k < NQ_COUNT; k++) {
        const nqt_t *t = &q->t[k];
        char where[80];
        if (k == NQ_ROUTER && t->addr[0] && q->iface[0])
            sfmt(where, sizeof where, "%s on %s", t->addr, q->iface);
        else
            str_copy(where, sizeof where, t->addr[0] ? t->addr : k == NQ_ROUTER ? "no router (no default route)" : "...");
        if (t->noroute) {
            lc_kv(c, nm[k], CP(C_WARN), "%s: no network to reach it", where);
            continue;
        }
        if (t->avg_ms < 0) {
            lc_kv(c, nm[k], t->loss_pct > 0 ? CP(C_CRIT) | A_BOLD : A_GRAY, "%s%s", where,
                  t->loss_pct > 0 ? ": no answers" : "");
            continue;
        }
        char now[24];
        if (t->rtt_ms >= 0)
            sfmt(now, sizeof now, "%.1f ms now", t->rtt_ms);
        else
            str_copy(now, sizeof now, "the last one lost");
        lc_kv(c, nm[k], t->trouble ? CP(C_WARN) | A_BOLD : A_BOLD, "%s: %s, %.1f ms average (%.1f-%.1f, jitter %.1f)",
              where, now, t->avg_ms, t->min_ms, t->max_ms, MAX(t->jitter_ms, 0));
        lc_kv(c, "", t->loss_pct >= 10 ? CP(C_CRIT) | A_BOLD : t->loss_pct > 0 ? CP(C_WARN) : A_GRAY,
              "%.0f%% lost%s", MAX(t->loss_pct, 0), t->trouble == NQ_LOSS ? "  (trouble now)" : t->trouble == NQ_SLOW ? "  (slow now)" : "");
    }
}

/* what's plugged in, at what speed, from which port */
static void hw_usb(sys_t *s, lc_t *c)
{
    const usb_t *u = &s->usb;
    if (!u->ok)
        return;
    lc_title(c, "USB");
    if (!u->n) {
        lc_text(c, A_GRAY, "%s", "Nothing plugged in");
        return;
    }
    int iw = 4;
    for (int i = 0; i < u->n; i++)
        iw = MAX(iw, (int)strlen(u->d[i].id));
    iw = MIN(iw, 10);
    int nw = CLAMP(c->w - iw - 43, 12, 30);
    lc_text(c, A_GRAY, "%-*s %-*s %-14s %-10s %s", iw, "Port", nw, "Device", "Kind", "Speed", "Where");
    for (int i = 0; i < u->n; i++) {
        const usbdev_t *d = &u->d[i];
        char sp[24], kind[40];
        usb_speed(d->mbps, sp, sizeof sp);
        if (d->blk[0])
            sfmt(kind, sizeof kind, "%s %s", d->kind, d->blk);
        else
            str_copy(kind, sizeof kind, d->kind);
        const char *w = d->where == 1 ? "socket" : d->where == 2 ? "built in" : d->where == 3 ? "via hub" : "";
        lc_text(c, d->slow ? CP(C_WARN) | A_BOLD : A_NORMAL, "%-*s %-*.*s %-14s %-10s %s", iw, d->id, nw, nw, d->name,
                kind, sp, w);
        if (d->slow)
            lc_text(c, CP(C_WARN), "%*s runs at USB 2 speed, can do %dG: %s", iw, "", d->can / 1000,
                    d->port3 == 1 ? "a USB 2 cable, or the plug not in all the way?"
                    : d->depth && !d->port3 ? "it's behind a USB 2 hub"
                    : !d->port3             ? "this port is USB 2 only, try another"
                                            : "a USB 2 port, hub or cable on the way");
    }
}

void hardware(sys_t *s, ui_state_t *u)
{
    int top = 1, h = LINES - 2, inner = h - 2;
    u->hw_scroll = CLAMP(u->hw_scroll, 0, u->hw_max);
    if (COLS >= 110) {
        int lw = COLS / 2;
        box_(top, 0, h, lw, "Health · Sensors · Graphics · Power", NULL);
        box_(top, lw, h, COLS - lw, "System · Storage · Network · USB", NULL);
        lc_t L = { top + 1, 2, lw - 4, inner, 0, u->hw_scroll };
        lc_t R = { top + 1, lw + 2, COLS - lw - 4, inner, 0, u->hw_scroll };
        hw_health(s, &L);
        hw_sensors(s, &L);
        hw_gpus(s, &L);
        hw_power(s, &L);
        hw_system(s, &R);
        hw_storage(s, &R);
        hw_network(s, &R);
        hw_usb(s, &R);
        u->hw_max = MAX(0, MAX(L.line, R.line) - inner);
    } else {
        box_(top, 0, h, COLS, "Hardware", NULL);
        lc_t C = { top + 1, 2, COLS - 4, inner, 0, u->hw_scroll };
        hw_health(s, &C);
        hw_system(s, &C);
        hw_sensors(s, &C);
        hw_gpus(s, &C);
        hw_power(s, &C);
        hw_storage(s, &C);
        hw_network(s, &C);
        hw_usb(s, &C);
        u->hw_max = MAX(0, C.line - inner);
    }
    if (u->hw_max > 0) {
        char m[64];
        sfmt(m, sizeof m, " %s scroll %d/%d ", U ? "↑↓" : "up/down", u->hw_scroll, u->hw_max);
        put(top + h - 1, COLS - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}

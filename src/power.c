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

/* power.c - CPU/DRAM/iGPU power from RAPL (powercap) and battery drain */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void power_init(power_t *p)
{
    char base[PATHBUF], dir[PATHBUF], f[PATHBUF];
    p->ac_online = -1;
    p->pkg_w = p->core_w = p->uncore_w = p->dram_w = p->psys_w = -1;

    /* RAPL zones: intel-rapl:0 = package 0, intel-rapl:0:0 = its cores, ...
     * Recent kernels expose the same interface for AMD Zen CPUs too.
     * (intel-rapl-mmio duplicates the package zone, so it is skipped.) */
    sfmt(base, sizeof base, "%s/class/powercap", g_sysfs);
    DIR *d = opendir(base);
    if (d) {
        static char names[64][64];
        int nn = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nn < 64)
            if (!strncmp(e->d_name, "intel-rapl:", 11))
                str_copy(names[nn++], sizeof names[0], e->d_name);
        closedir(d);
        qsort(names, (size_t)nn, sizeof names[0], cmp_name);

        for (int i = 0; i < nn && p->nrapl < MAX_RAPL; i++) {
            rapl_t *z = &p->z[p->nrapl];
            memset(z, 0, sizeof *z);
            sfmt(dir, sizeof dir, "%s/%s", base, names[i]);
            sfmt(f, sizeof f, "%s/name", dir);
            if (read_line(f, z->name, sizeof z->name))
                continue;
            sfmt(z->path, sizeof z->path, "%s/energy_uj", dir);
            if (!path_exists(z->path))
                continue;
            sfmt(f, sizeof f, "%s/max_energy_range_uj", dir);
            if (read_ull(f, &z->max_range))
                z->max_range = 0;
            int colons = 0;
            for (const char *q = names[i]; *q; q++)
                colons += *q == ':';
            z->depth = colons - 1;
            /* since Linux 5.10 energy_uj is root-only (PLATYPUS side channel) */
            int fd = open(z->path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                z->readable = 1;
                close(fd);
            } else {
                p->rapl_denied = 1;
            }
            p->nrapl++;
        }
    }

    /* batteries and AC adapter */
    sfmt(base, sizeof base, "%s/class/power_supply", g_sysfs);
    d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            char type[32], scope[32];
            sfmt(dir, sizeof dir, "%s/%s", base, e->d_name);
            sfmt(f, sizeof f, "%s/type", dir);
            if (read_line(f, type, sizeof type))
                continue;
            sfmt(f, sizeof f, "%s/scope", dir);
            if (read_line(f, scope, sizeof scope))
                scope[0] = '\0';
            if (!strcmp(type, "Battery") && strcmp(scope, "Device") && p->nbat < 4) {
                battery_t *b = &p->bat[p->nbat++];
                str_copy(b->name, sizeof b->name, e->d_name);
                str_copy(b->path, sizeof b->path, dir);
                b->watts = b->capacity = b->time_left = b->health = b->full_wh = b->design_wh = -1;
                b->energy_wh = b->energy_full_wh = -1;
                b->cycles = -1;
                b->lim_end = b->lim_start = -1;
            } else if (!strcmp(type, "Mains") && !p->ac_path[0]) {
                sfmt(p->ac_path, sizeof p->ac_path, "%s/online", dir);
            } else if (!strcmp(type, "USB") && strcmp(scope, "Device") && p->nusbc < 4) {
                sfmt(p->usbc_path[p->nusbc++], sizeof p->usbc_path[0], "%s/online", dir);
            }
        }
        closedir(d);
    }
}

static int battr(const battery_t *b, const char *name, long long *v)
{
    char f[PATHBUF];
    sfmt(f, sizeof f, "%s/%s", b->path, name);
    return read_ll(f, v);
}

static void battery_sample(battery_t *b)
{
    char f[PATHBUF];
    long long v, cur, volt, now, full;
    sfmt(f, sizeof f, "%s/status", b->path);
    if (read_line(f, b->status, sizeof b->status))
        str_copy(b->status, sizeof b->status, "Unknown");
    b->capacity = battr(b, "capacity", &v) == 0 ? (double)v : -1;

    b->watts = -1;
    if (battr(b, "power_now", &v) == 0)
        b->watts = fabs((double)v) / 1e6;
    else if (battr(b, "current_now", &cur) == 0 && battr(b, "voltage_now", &volt) == 0)
        b->watts = fabs((double)cur) * (double)volt / 1e12;

    double e_now = -1, e_full = -1; /* Wh */
    if (battr(b, "energy_now", &now) == 0 && battr(b, "energy_full", &full) == 0) {
        e_now = (double)now / 1e6;
        e_full = (double)full / 1e6;
    } else if (battr(b, "charge_now", &now) == 0 && battr(b, "charge_full", &full) == 0 &&
               (battr(b, "voltage_min_design", &volt) == 0 || battr(b, "voltage_now", &volt) == 0)) {
        e_now = (double)now * (double)volt / 1e12;
        e_full = (double)full * (double)volt / 1e12;
    }
    b->energy_wh = e_now;
    b->energy_full_wh = e_full > 0 ? e_full : -1;
    /* wear: what a full charge holds now compared with when it was new */
    long long des, fl;
    b->health = b->full_wh = b->design_wh = -1;
    if (battr(b, "energy_full_design", &des) == 0 && battr(b, "energy_full", &fl) == 0) {
        if (des > 0 && fl > 0) {
            b->health = CLAMP(100.0 * (double)fl / (double)des, 0, 150);
            b->full_wh = (double)fl / 1e6;
            b->design_wh = (double)des / 1e6;
        }
    } else if (battr(b, "charge_full_design", &des) == 0 && battr(b, "charge_full", &fl) == 0) {
        if (des > 0 && fl > 0) {
            b->health = CLAMP(100.0 * (double)fl / (double)des, 0, 150);
            if (battr(b, "voltage_min_design", &volt) == 0 && volt > 0) {
                b->full_wh = (double)fl * (double)volt / 1e12;
                b->design_wh = (double)des * (double)volt / 1e12;
            }
        }
    }
    b->cycles = battr(b, "cycle_count", &v) == 0 && v > 0 ? (long)v : -1;

    /* a charge limit, if the laptop has one (ASUS, ThinkPad, Dell, Huawei...);
     * it hardly ever changes and some firmware is slow to answer: once a minute */
    double mono = now_sec();
    if (!b->lim_t || mono - b->lim_t >= 60) {
        b->lim_t = mono;
        long long e = -1, s = -1;
        int he = 0;
        static const char *const ends[] = { "charge_control_end_threshold", "charge_stop_threshold" };
        for (int k = 0; k < 2 && !he; k++)
            he = battr(b, ends[k], &e) == 0;
        int hs = battr(b, "charge_control_start_threshold", &s) == 0 || battr(b, "charge_start_threshold", &s) == 0;
        b->lim_ok = he;
        b->lim_end = he && e > 0 && e < 100 ? (int)e : -1;
        b->lim_start = hs && s > 0 && s < 100 ? (int)s : -1;
    }

    b->time_left = -1;
    if (b->watts > 0.1 && e_now >= 0) {
        if (!strcmp(b->status, "Discharging"))
            b->time_left = e_now / b->watts * 3600.0;
        else if (!strcmp(b->status, "Charging") && e_full > e_now)
            b->time_left = (e_full - e_now) / b->watts * 3600.0;
    }
}

/* ------------------------------------------------ the CPU's power limits */

/* one interface's limits for package 0: sustained (and its window) and short-term */
static void lim_read(const char *dir, double *lw, double *ls, double *sw, int *off)
{
    char f[PATHBUF], nm[32];
    long long v;
    *lw = *ls = *sw = -1;
    *off = 0;
    sfmt(f, sizeof f, "%s/name", dir);
    if (read_line(f, nm, sizeof nm) || strcmp(nm, "package-0"))
        return;
    sfmt(f, sizeof f, "%s/enabled", dir); /* the sustained limit's on/off switch */
    *off = read_ll(f, &v) == 0 && v == 0;
    for (int k = 0; k < 4; k++) {
        sfmt(f, sizeof f, "%s/constraint_%d_name", dir, k);
        if (read_line(f, nm, sizeof nm))
            continue;
        sfmt(f, sizeof f, "%s/constraint_%d_power_limit_uw", dir, k);
        if (read_ll(f, &v) || v <= 0)
            continue;
        if (!strcmp(nm, "long_term")) {
            *lw = (double)v / 1e6;
            sfmt(f, sizeof f, "%s/constraint_%d_time_window_us", dir, k);
            if (read_ll(f, &v) == 0 && v > 0)
                *ls = (double)v / 1e6;
        } else if (!strcmp(nm, "short_term")) {
            *sw = (double)v / 1e6;
        }
    }
}

static double lower(double a, double b)
{
    return a > 0 && b > 0 ? MIN(a, b) : a > 0 ? a : b;
}

/* they change with the platform profile or the firmware's mood: every 10 s */
static void limits_sample(power_t *p)
{
    cpulim_t *L = &p->lim;
    double mono = now_sec(), fs;
    if (L->t && mono - L->t < 10)
        return;
    L->t = mono;
    char d[PATHBUF];
    int fw_off;
    sfmt(d, sizeof d, "%s/class/powercap/intel-rapl:0", g_sysfs);
    lim_read(d, &L->long_w, &L->long_s, &L->short_w, &L->long_off);
    sfmt(d, sizeof d, "%s/class/powercap/intel-rapl-mmio:0", g_sysfs);
    lim_read(d, &L->fw_long_w, &fs, &L->fw_short_w, &fw_off);
    if (fw_off)
        L->fw_long_w = -1;
    if (L->long_s <= 0)
        L->long_s = fs;
    L->eff_long_w = lower(L->long_off ? -1 : L->long_w, L->fw_long_w);
    L->eff_short_w = lower(L->short_w, L->fw_short_w);
    L->ok = L->long_w > 0 || L->fw_long_w > 0;
}

/* the slowest part of this sample so far (for "reading X took 3 s" events) */
static void note_slow(power_t *p, double t0, const char *what)
{
    double d = now_sec() - t0;
    if (d > p->slow_s) {
        p->slow_s = d;
        str_copy(p->slow, sizeof p->slow, what);
    }
}

void power_sample(power_t *p, double dt)
{
    (void)dt; /* each meter keeps the time of its own last read instead */
    double pkg = 0, core = 0, unc = 0, dram = 0, psys = 0;
    int hp = 0, hc = 0, hu = 0, hd = 0, hs = 0;
    p->slow_s = 0;
    p->slow[0] = '\0';
    double t0 = now_sec();
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        unsigned long long e;
        if (!z->readable || read_ull(z->path, &e)) {
            z->watts = -1;
            continue;
        }
        /* Energy divided by the time between this read and the last one,
         * both taken right here: if something earlier in the sample was
         * slow, a stall can't turn into a fake spike of power. */
        double t = now_sec();
        if (!z->primed) {
            z->watts = -1;
            z->prev = e;
            z->prev_t = t;
            z->primed = 1;
        } else if (t - z->prev_t >= 0.2) {
            unsigned long long de;
            if (e >= z->prev)
                de = e - z->prev;
            else /* counter wrapped */
                de = z->max_range > z->prev ? z->max_range - z->prev + e : 0;
            z->watts = (double)de / 1e6 / (t - z->prev_t);
            z->prev = e;
            z->prev_t = t;
        } /* else read again right away: too short to measure, keep the last figure */
        if (z->watts < 0)
            continue;
        if (!strncmp(z->name, "package", 7) && z->depth == 0) {
            pkg += z->watts;
            hp = 1;
        } else if (!strcmp(z->name, "core")) {
            core += z->watts;
            hc = 1;
        } else if (!strcmp(z->name, "uncore")) {
            unc += z->watts;
            hu = 1;
        } else if (!strcmp(z->name, "dram")) {
            dram += z->watts;
            hd = 1;
        } else if (!strcmp(z->name, "psys")) {
            psys += z->watts;
            hs = 1;
        }
    }
    p->pkg_w = hp ? pkg : -1;
    p->core_w = hc ? core : -1;
    p->uncore_w = hu ? unc : -1;
    p->dram_w = hd ? dram : -1;
    p->psys_w = hs ? psys : -1;
    /* psys covers the whole platform, the CPU package included: well below
     * the package means the firmware never connected it (0.08 W next to an
     * 8 W CPU on some laptops). A few samples in a row, then it's ignored. */
    if (hs && hp && pkg > 1.0 && !p->psys_bad) {
        p->psys_low = psys < 0.9 * pkg ? p->psys_low + 1 : 0;
        if (p->psys_low >= 3)
            p->psys_bad = 1;
    }
    if (p->nrapl) {
        limits_sample(p);
        note_slow(p, t0, "the CPU power meters (RAPL)");
    }

    for (int i = 0; i < p->nbat; i++) {
        char what[40];
        double tb = now_sec();
        battery_sample(&p->bat[i]);
        sfmt(what, sizeof what, "battery %s", p->bat[i].name);
        note_slow(p, tb, what);
    }
    if (p->ac_path[0]) {
        long long v;
        double ta = now_sec();
        p->ac_online = read_ll(p->ac_path, &v) == 0 ? (int)v : -1;
        note_slow(p, ta, "the AC adapter");
    } else if (p->nusbc) { /* USB-C only: plugged in when one of them charges */
        long long v;
        p->ac_online = -1;
        for (int i = 0; i < p->nusbc; i++)
            if (read_ll(p->usbc_path[i], &v) == 0)
                p->ac_online = MAX(p->ac_online, v > 0 ? 1 : 0);
    }
}

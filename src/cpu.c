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

/* cpu.c - CPU usage (/proc/stat), clocks (cpufreq), load and topology */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_buf;
static size_t g_cap;

static void strip_all(char *s, const char *junk)
{
    size_t jl = strlen(junk);
    char *p;
    while ((p = strstr(s, junk)) != NULL)
        memmove(p, p + jl, strlen(p + jl) + 1);
}

/* "Intel(R) Core(TM) i5-8500 CPU @ 3.00GHz" -> "Intel Core i5-8500 @ 3.00GHz" */
static void clean_model(char *s)
{
    static const char *const junk[] = { "(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor", NULL };
    for (int i = 0; junk[i]; i++)
        strip_all(s, junk[i]);
    char *r = s, *w = s;
    int sp = 1;
    for (; *r; r++) {
        if (isspace((unsigned char)*r)) {
            if (!sp)
                *w++ = ' ';
            sp = 1;
        } else {
            *w++ = *r;
            sp = 0;
        }
    }
    while (w > s && w[-1] == ' ')
        w--;
    *w = '\0';
}

static void add_pair(int pairs[][2], int *n, int a, int b)
{
    for (int i = 0; i < *n; i++)
        if (pairs[i][0] == a && pairs[i][1] == b)
            return;
    if (*n < MAX_CPUS) {
        pairs[*n][0] = a;
        pairs[*n][1] = b;
        (*n)++;
    }
}

/* an ARM core's name from cpuinfo's implementer and part: "Cortex-A72", or NULL */
static const char *arm_part(unsigned impl, unsigned part, const char **maker)
{
    static const struct {
        unsigned impl, part;
        const char *name;
    } P[] = { { 0x41, 0xd03, "Cortex-A53" },    { 0x41, 0xd04, "Cortex-A35" },    { 0x41, 0xd05, "Cortex-A55" },
              { 0x41, 0xd07, "Cortex-A57" },    { 0x41, 0xd08, "Cortex-A72" },    { 0x41, 0xd09, "Cortex-A73" },
              { 0x41, 0xd0a, "Cortex-A75" },    { 0x41, 0xd0b, "Cortex-A76" },    { 0x41, 0xd0c, "Neoverse-N1" },
              { 0x41, 0xd0d, "Cortex-A77" },    { 0x41, 0xd40, "Neoverse-V1" },   { 0x41, 0xd41, "Cortex-A78" },
              { 0x41, 0xd44, "Cortex-X1" },     { 0x41, 0xd46, "Cortex-A510" },   { 0x41, 0xd47, "Cortex-A710" },
              { 0x41, 0xd48, "Cortex-X2" },     { 0x41, 0xd49, "Neoverse-N2" },   { 0x41, 0xd4b, "Cortex-A78C" },
              { 0x41, 0xd4d, "Cortex-A715" },   { 0x41, 0xd4e, "Cortex-X3" },     { 0x41, 0xd4f, "Neoverse-V2" },
              { 0x41, 0xd80, "Cortex-A520" },   { 0x41, 0xd81, "Cortex-A720" },   { 0x41, 0xd82, "Cortex-X4" },
              { 0x41, 0xd84, "Neoverse-V3" },   { 0x41, 0xd85, "Cortex-X925" },   { 0x41, 0xd87, "Cortex-A725" },
              { 0x41, 0xd8e, "Neoverse-N3" },   { 0x51, 0x001, "Oryon" },         { 0xc0, 0xac3, "AmpereOne" },
              { 0x48, 0xd01, "Kunpeng 920" },   { 0x61, 0x022, "M1 Icestorm" },   { 0x61, 0x023, "M1 Firestorm" },
              { 0x61, 0x032, "M2 Blizzard" },   { 0x61, 0x033, "M2 Avalanche" } };
    static const struct {
        unsigned impl;
        const char *name;
    } M[] = { { 0x41, "ARM" }, { 0x51, "Qualcomm" }, { 0x61, "Apple" }, { 0xc0, "Ampere" }, { 0x48, "HiSilicon" },
              { 0x4e, "NVIDIA" }, { 0x46, "Fujitsu" } };
    *maker = NULL;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (M[i].impl == impl)
            *maker = M[i].name;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (P[i].impl == impl && P[i].part == part)
            return P[i].name;
    return NULL;
}

void cpu_init(cpu_t *c)
{
    char path[PATHBUF];
    c->temp_c = -1;
    c->turbo = -1;
    c->lim_min = c->lim_max = c->hw_min = c->hw_max = -1;
    str_copy(c->model, sizeof c->model, "Unknown CPU");

    sfmt(path, sizeof path, "%s/cpuinfo", g_procfs);
    char *buf = NULL;
    size_t cap = 0;
    int threads = 0, npairs = 0;
    static int pairs[MAX_CPUS][2];
    if (read_file_grow(path, &buf, &cap) > 0) {
        int prio = 0, phys = 0, core = -1;
        unsigned impl = 0;
        char arm[96] = ""; /* big.LITTLE: "ARM Cortex-A76 + Cortex-A55" */
        char *cur = buf, *line;
        while ((line = next_line(&cur)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon) { /* blank line ends one processor block */
                if (core >= 0)
                    add_pair(pairs, &npairs, phys, core);
                phys = 0;
                core = -1;
                continue;
            }
            char *ke = colon;
            while (ke > line && isspace((unsigned char)ke[-1]))
                ke--;
            *ke = '\0';
            char *val = colon + 1;
            while (isspace((unsigned char)*val))
                val++;

            if (!strcmp(line, "processor"))
                threads++;
            else if (!strcmp(line, "physical id"))
                phys = atoi(val);
            else if (!strcmp(line, "core id"))
                core = atoi(val);
            else if (!strcmp(line, "model name") && prio < 3) {
                str_copy(c->model, sizeof c->model, val);
                prio = strncmp(val, "ARMv", 4) ? 3 : 1; /* "ARMv7 Processor rev 3 (v7l)": the part says more */
            } else if (!strcmp(line, "CPU implementer")) {
                impl = (unsigned)strtoul(val, NULL, 0);
            } else if (!strcmp(line, "CPU part") && prio < 3) {
                const char *maker, *nm = arm_part(impl, (unsigned)strtoul(val, NULL, 0), &maker);
                if (nm && !strstr(arm, nm)) {
                    if (!arm[0])
                        sfmt(arm, sizeof arm, "%s %s", maker ? maker : "", nm);
                    else if (strlen(arm) + strlen(nm) + 4 < sizeof arm)
                        sfmt(arm + strlen(arm), sizeof arm - strlen(arm), " + %s", nm);
                    str_copy(c->model, sizeof c->model, arm[0] == ' ' ? arm + 1 : arm);
                    prio = 2;
                }
            } else if ((!strcmp(line, "Model") || !strcmp(line, "Hardware") || !strcmp(line, "cpu model")) &&
                       prio < 1 && *val) {
                str_copy(c->model, sizeof c->model, val);
                prio = 1;
            }
        }
        if (core >= 0)
            add_pair(pairs, &npairs, phys, core);
    }
    free(buf);
    clean_model(c->model);
    c->threads = threads;
    c->cores = npairs ? npairs : threads;
}

/* Read once per CPU: topology (for per-core temperatures) and max clock. */
static void cpu_topology(cpu_t *c, int id)
{
    char p[PATHBUF];
    long long v;
    c->topo_done[id] = 1;
    c->core_id[id] = c->pkg_id[id] = -1;
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/core_id", g_sysfs, id);
    if (!read_ll(p, &v))
        c->core_id[id] = (int)v;
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/physical_package_id", g_sysfs, id);
    if (!read_ll(p, &v))
        c->pkg_id[id] = (int)v;
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", g_sysfs, id);
    if (!read_ll(p, &v) && v > 0)
        c->mhz_hw[id] = (double)v / 1000.0;
}

static double jtot(const cpu_jiffies_t *j)
{
    return j->user + j->nice + j->sys + j->idle + j->iowait + j->irq + j->softirq + j->steal;
}

static double busy_pct(const cpu_jiffies_t *a, const cpu_jiffies_t *b)
{
    double d = jtot(b) - jtot(a);
    if (d <= 0)
        return 0;
    double di = (b->idle + b->iowait) - (a->idle + a->iowait);
    return CLAMP(100.0 * (d - di) / d, 0.0, 100.0);
}

static double ull_rate(unsigned long long prev, unsigned long long cur, double dt)
{
    return (dt > 0 && cur >= prev) ? (double)(cur - prev) / dt : 0;
}

/* Fallback when cpufreq is unavailable (VMs, some ARM boards). */
static int freqs_from_cpuinfo(cpu_t *c)
{
    static char *buf;
    static size_t cap;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/cpuinfo", g_procfs);
    if (read_file_grow(path, &buf, &cap) <= 0)
        return 0;
    int cur_id = -1, found = 0;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (!strncmp(line, "processor", 9)) {
            char *col = strchr(line, ':');
            cur_id = col ? atoi(col + 1) : -1;
        } else if (!strncmp(line, "cpu MHz", 7)) {
            char *col = strchr(line, ':');
            if (!col)
                continue;
            double mhz = atof(col + 1);
            for (int i = 0; i < c->ncpu; i++)
                if (c->id[i] == cur_id) {
                    c->mhz[i] = mhz;
                    found = 1;
                }
        }
    }
    return found;
}

/* Idle states (cpuidle): how long the CPUs sleep, and how deeply. Every
 * state of every CPU is two small files, so this runs every 2 s at most. */
static void cpu_idle_sample(cpu_t *c)
{
    char p[PATHBUF];
    double t = now_sec();
    if (c->idle_primed >= 2 && t - c->idle_t < 2.0) /* the first two reads go straight through */
        return;
    if (!c->nidle && c->idle_t == 0) { /* names, from the first CPU */
        for (int k = 0; k < MAX_CSTATES; k++) {
            cstate_t *st = &c->idle[k];
            sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpuidle/state%d/name", g_sysfs, c->ncpu ? c->id[0] : 0, k);
            if (read_line(p, st->name, sizeof st->name))
                break;
            long long v;
            sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpuidle/state%d/latency", g_sysfs, c->ncpu ? c->id[0] : 0, k);
            st->latency = !read_ll(p, &v) && v >= 0 ? (unsigned)v : 0;
            c->nidle = k + 1;
        }
    }
    double el = c->idle_t > 0 ? t - c->idle_t : 0;
    c->idle_t = t;
    if (!c->nidle)
        return;
    double sum[MAX_CSTATES] = { 0 }, entries = 0;
    int ncount = 0;
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i];
        if (id < 0 || id >= MAX_CPUS)
            continue;
        ncount++;
        for (int k = 0; k < c->nidle; k++) {
            unsigned long long tm, us;
            sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpuidle/state%d/time", g_sysfs, id, k);
            if (read_ull(p, &tm))
                continue;
            sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpuidle/state%d/usage", g_sysfs, id, k);
            if (read_ull(p, &us))
                us = c->idle_use[id][k];
            if (c->idle_primed && el > 0) {
                if (tm >= c->idle_time[id][k])
                    sum[k] += (double)(tm - c->idle_time[id][k]);
                if (us >= c->idle_use[id][k])
                    entries += (double)(us - c->idle_use[id][k]);
            }
            c->idle_time[id][k] = tm;
            c->idle_use[id][k] = us;
        }
    }
    if (c->idle_primed && el > 0 && ncount) {
        for (int k = 0; k < c->nidle; k++) /* time is in us */
            c->idle[k].pct = CLAMP(sum[k] / (el * 1e6 * ncount) * 100.0, 0, 100);
        c->wakeups = entries / el;
        c->idle_primed = 2;
    }
    for (int k = 0; k < c->nidle; k++) {
        long long v;
        sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpuidle/state%d/disable", g_sysfs, c->ncpu ? c->id[0] : 0, k);
        c->idle[k].disabled = !read_ll(p, &v) && v > 0;
    }
    if (!c->idle_primed)
        c->idle_primed = 1;
}

/* Governor, driver, energy preference, turbo, frequency limits and the
 * ACPI platform profile. These only change when someone changes them. */
void cpu_power_sample(cpu_t *c)
{
    char p[PATHBUF];
    long long v;
    int id = c->ncpu ? c->id[0] : 0;
    const char *cf = "devices/system/cpu";
#define RD(field, fmt, ...)                                                                                          \
    do {                                                                                                             \
        sfmt(p, sizeof p, fmt, __VA_ARGS__);                                                                         \
        if (read_line(p, c->field, sizeof c->field))                                                                 \
            c->field[0] = '\0';                                                                                      \
    } while (0)
    RD(driver, "%s/%s/cpu%d/cpufreq/scaling_driver", g_sysfs, cf, id);
    RD(governor, "%s/%s/cpu%d/cpufreq/scaling_governor", g_sysfs, cf, id);
    RD(epp, "%s/%s/cpu%d/cpufreq/energy_performance_preference", g_sysfs, cf, id);
    RD(pstate, "%s/%s/intel_pstate/status", g_sysfs, cf);
    RD(profile, "%s/firmware/acpi/platform_profile", g_sysfs);
#undef RD
    /* turbo: intel_pstate says "no_turbo", acpi-cpufreq / amd-pstate say "boost" */
    c->turbo = -1;
    sfmt(p, sizeof p, "%s/%s/intel_pstate/no_turbo", g_sysfs, cf);
    if (!read_ll(p, &v)) {
        c->turbo = v ? 0 : 1;
    } else {
        sfmt(p, sizeof p, "%s/%s/cpufreq/boost", g_sysfs, cf);
        if (!read_ll(p, &v))
            c->turbo = v ? 1 : 0;
        else {
            sfmt(p, sizeof p, "%s/%s/cpu%d/cpufreq/boost", g_sysfs, cf, id);
            if (!read_ll(p, &v))
                c->turbo = v ? 1 : 0;
        }
    }
    static const char *const f[4] = { "scaling_min_freq", "scaling_max_freq", "cpuinfo_min_freq", "cpuinfo_max_freq" };
    double *out[4] = { &c->lim_min, &c->lim_max, &c->hw_min, &c->hw_max };
    for (int k = 0; k < 4; k++) {
        sfmt(p, sizeof p, "%s/%s/cpu%d/cpufreq/%s", g_sysfs, cf, id, f[k]);
        *out[k] = !read_ll(p, &v) && v > 0 ? (double)v / 1000.0 : -1;
    }
}

void cpu_sample(cpu_t *c, double dt)
{
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/stat", g_procfs);
    if (read_file_grow(path, &g_buf, &g_cap) <= 0)
        return;

    int n = 0;
    unsigned long long ctxt = 0, intr = 0, forks = 0;
    char *cur = g_buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u') {
            char *p = line + 3;
            int id = -1;
            if (isdigit((unsigned char)*p))
                id = (int)strtol(p, &p, 10);
            unsigned long long v[8] = { 0 };
            if (sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                       &v[6], &v[7]) < 4)
                continue;
            cpu_jiffies_t j = { (double)v[0], (double)v[1], (double)v[2], (double)v[3],
                                (double)v[4], (double)v[5], (double)v[6], (double)v[7] };
            if (id < 0) {
                const cpu_jiffies_t *a = &c->prev_all;
                double d = jtot(&j) - jtot(a);
                if (c->primed && d > 0) {
                    c->total = busy_pct(a, &j);
                    c->user = CLAMP(100.0 * ((j.user + j.nice) - (a->user + a->nice)) / d, 0.0, 100.0);
                    c->sys = CLAMP(100.0 * ((j.sys + j.irq + j.softirq) - (a->sys + a->irq + a->softirq)) / d, 0.0,
                                   100.0);
                    c->iowait = CLAMP(100.0 * (j.iowait - a->iowait) / d, 0.0, 100.0);
                    c->steal = CLAMP(100.0 * (j.steal - a->steal) / d, 0.0, 100.0);
                }
                c->prev_all = j;
            } else if (n < MAX_CPUS && id >= 0 && id < MAX_CPUS) {
                c->id[n] = id;
                if (!c->topo_done[id])
                    cpu_topology(c, id);
                c->pct[n] = c->primed ? busy_pct(&c->prev[id], &j) : 0;
                c->prev[id] = j;
                n++;
            }
        } else if (!strncmp(line, "ctxt ", 5)) {
            ctxt = strtoull(line + 5, NULL, 10);
        } else if (!strncmp(line, "intr ", 5)) {
            intr = strtoull(line + 5, NULL, 10);
        } else if (!strncmp(line, "processes ", 10)) {
            forks = strtoull(line + 10, NULL, 10);
        } else if (!strncmp(line, "procs_running ", 14)) {
            c->procs_running = (unsigned)strtoul(line + 14, NULL, 10);
        } else if (!strncmp(line, "procs_blocked ", 14)) {
            c->procs_blocked = (unsigned)strtoul(line + 14, NULL, 10);
        }
    }
    c->ncpu = n;
    if (c->primed) {
        c->ctxt_rate = ull_rate(c->prev_ctxt, ctxt, dt);
        c->intr_rate = ull_rate(c->prev_intr, intr, dt);
        c->fork_rate = ull_rate(c->prev_forks, forks, dt);
    }
    c->prev_ctxt = ctxt;
    c->prev_intr = intr;
    c->prev_forks = forks;

    /* clocks: cpufreq in sysfs (kHz), else "cpu MHz" in /proc/cpuinfo */
    if (c->freq_src == 0 || c->freq_src == 1) {
        int got = 0;
        for (int i = 0; i < n; i++) {
            long long khz;
            sfmt(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", g_sysfs, c->id[i]);
            if (read_ll(path, &khz) == 0) {
                c->mhz[i] = (double)khz / 1000.0;
                got = 1;
            } else {
                c->mhz[i] = 0;
            }
        }
        if (got)
            c->freq_src = 1;
        else if (c->freq_src == 0)
            c->freq_src = 2;
    }
    if (c->freq_src == 2 && !freqs_from_cpuinfo(c))
        c->freq_src = -1;

    double sum = 0, mx = 0;
    int cnt = 0;
    for (int i = 0; i < n; i++)
        if (c->mhz[i] > 0) {
            sum += c->mhz[i];
            cnt++;
            if (c->mhz[i] > mx)
                mx = c->mhz[i];
        }
    c->mhz_avg = cnt ? sum / cnt : 0;
    c->mhz_max = mx;

    cpu_idle_sample(c);

    char b[256];
    sfmt(path, sizeof path, "%s/loadavg", g_procfs);
    if (read_file(path, b, sizeof b) > 0)
        sscanf(b, "%lf %lf %lf", &c->load[0], &c->load[1], &c->load[2]);
    sfmt(path, sizeof path, "%s/uptime", g_procfs);
    if (read_file(path, b, sizeof b) > 0)
        sscanf(b, "%lf", &c->uptime);

    c->primed = 1;
}

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

/* sensors.c - temperatures, fans and power readings from /sys/class/hwmon */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int type, num, pref;
    char file[64];
} cand_t;

static int match_attr(const char *nm, const char *prefix, const char *suffix, int *num)
{
    size_t pl = strlen(prefix), sl = strlen(suffix), nl = strlen(nm);
    if (nl <= pl + sl || strncmp(nm, prefix, pl) || strcmp(nm + nl - sl, suffix))
        return 0;
    char *end;
    long v = strtol(nm + pl, &end, 10);
    if (end == nm + pl || end != nm + nl - sl)
        return 0;
    *num = (int)v;
    return 1;
}

static int cand_cmp(const void *a, const void *b)
{
    const cand_t *x = a, *y = b;
    if (x->type != y->type)
        return x->type - y->type;
    if (x->num != y->num)
        return x->num - y->num;
    return x->pref - y->pref;
}

static int scan_chip(sensors_t *S, const char *dir, const char *chip, int chip_idx)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    static cand_t c[256];
    int nc = 0, num;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nc < 256) {
        const char *nm = e->d_name;
        int type = -1, pref = 0;
        if (match_attr(nm, "temp", "_input", &num))
            type = SENS_TEMP;
        else if (match_attr(nm, "fan", "_input", &num))
            type = SENS_FAN;
        else if (match_attr(nm, "power", "_average", &num))
            type = SENS_POWER;
        else if (match_attr(nm, "power", "_input", &num)) {
            type = SENS_POWER;
            pref = 1;
        } else if (match_attr(nm, "in", "_input", &num))
            type = SENS_VOLT;
        if (type < 0)
            continue;
        c[nc].type = type;
        c[nc].num = num;
        c[nc].pref = pref;
        str_copy(c[nc].file, sizeof c[nc].file, nm);
        nc++;
    }
    closedir(d);
    qsort(c, (size_t)nc, sizeof *c, cand_cmp);

    static const char *const prefix[] = { "temp", "fan", "power", "in" };
    int added = 0;
    for (int i = 0; i < nc && S->n < MAX_SENSORS; i++) {
        if (i && c[i].type == c[i - 1].type && c[i].num == c[i - 1].num)
            continue; /* power*_average preferred over power*_input */
        sensor_t *s = &S->s[S->n];
        memset(s, 0, sizeof *s);
        s->type = c[i].type;
        s->chip_idx = chip_idx;
        s->crit = -1;
        str_copy(s->chip, sizeof s->chip, chip);
        sfmt(s->path, sizeof s->path, "%s/%s", dir, c[i].file);

        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%s%d_label", dir, prefix[s->type], c[i].num);
        if (read_line(p, s->label, sizeof s->label) || !s->label[0])
            sfmt(s->label, sizeof s->label, "%s%d", prefix[s->type], c[i].num);
        for (char *q = s->label; *q; q++) /* '|' separates the fields of a "sensor =" line */
            if (*q == '|' || (unsigned char)*q < 32)
                *q = '/';
        sfmt(s->id, sizeof s->id, "%s/%s", chip, s->label);
        for (int k = S->n - added, dup = 1; k < S->n; k++) /* the same label twice on one chip */
            if (!strcmp(S->s[k].id, s->id))
                sfmt(s->id, sizeof s->id, "%s/%s (%d)", chip, s->label, ++dup);
        if (s->type == SENS_FAN) {
            long long v;
            sfmt(p, sizeof p, "%s/fan%d_max", dir, c[i].num);
            if (!read_ll(p, &v) && v > 0)
                s->fmax = (double)v;
        }
        if (s->type == SENS_TEMP) {
            long long v;
            sfmt(p, sizeof p, "%s/temp%d_crit", dir, c[i].num);
            if (read_ll(p, &v) || v <= 0) {
                sfmt(p, sizeof p, "%s/temp%d_max", dir, c[i].num);
                if (read_ll(p, &v))
                    v = 0;
            }
            if (v > 0 && v < 200000)
                s->crit = (double)v / 1000.0;
        }
        if (s->type == SENS_VOLT) { /* limits set by the BIOS / driver, often unset (0) */
            long long lo, hi;
            sfmt(p, sizeof p, "%s/in%d_min", dir, c[i].num);
            if (!read_ll(p, &lo) && lo > 0)
                s->lo = (double)lo / 1000.0;
            sfmt(p, sizeof p, "%s/in%d_max", dir, c[i].num);
            if (!read_ll(p, &hi) && hi > 0)
                s->hi = (double)hi / 1000.0;
            if (s->hi > 0 && s->lo >= s->hi) /* nonsense pair */
                s->lo = s->hi = 0;
        }
        S->n++;
        added++;
    }
    return added;
}

static int cpu_score(const sensor_t *s)
{
    if (s->type != SENS_TEMP)
        return 0;
    const char *c = s->chip, *l = s->label;
    if (!strncmp(c, "coretemp", 8))
        return !strncmp(l, "Package", 7) ? 100 : 50;
    if (!strncmp(c, "k10temp", 7) || !strncmp(c, "zenpower", 8))
        return !strcmp(l, "Tctl") ? 100 : !strcmp(l, "Tdie") ? 95 : 60;
    if (!strncmp(c, "x86_pkg_temp", 12)) /* the package, as a thermal zone */
        return 90;
    if (!strncmp(c, "k8temp", 6) || strstr(c, "cputemp")) /* old AMD, VIA / Zhaoxin */
        return 50;
    if (!strncmp(c, "cpu", 3) || strstr(c, "cpu_thermal") || strstr(c, "soc_thermal"))
        return 40;
    if (!strncmp(c, "thinkpad", 8) && !strcmp(l, "CPU"))
        return 30;
    if (!strncasecmp(l, "CPU", 3) || !strncmp(l, "PECI", 4)) /* a board's or laptop's own CPU reading */
        return 20;
    if (!strncmp(c, "acpitz", 6))
        return 10;
    return 0;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

void sensors_init(sensors_t *S)
{
    char base[PATHBUF];
    S->n = 0;
    S->cpu_idx = -1;
    sfmt(base, sizeof base, "%s/class/hwmon", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    int ids[128], nid = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nid < 128)
        if (!strncmp(e->d_name, "hwmon", 5) && e->d_name[5])
            ids[nid++] = atoi(e->d_name + 5);
    closedir(d);
    qsort(ids, (size_t)nid, sizeof ids[0], cmp_int);

    char names[128][32];
    for (int i = 0; i < nid; i++) {
        char dir[PATHBUF], p[PATHBUF], name[32];
        sfmt(dir, sizeof dir, "%s/hwmon%d", base, ids[i]);
        sfmt(p, sizeof p, "%s/name", dir);
        if (read_line(p, name, sizeof name))
            sfmt(name, sizeof name, "hwmon%d", ids[i]);
        str_copy(names[i], sizeof names[i], name);
        int same = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(names[j], name))
                same++;
        char chip[32];
        if (same) {
            char shortname[24];
            str_copy(shortname, sizeof shortname, name);
            sfmt(chip, sizeof chip, "%s#%d", shortname, same + 1);
        } else {
            str_copy(chip, sizeof chip, name);
        }
        if (!scan_chip(S, dir, chip, i)) { /* very old kernels keep attributes in device/ */
            sfmt(p, sizeof p, "%s/device", dir);
            scan_chip(S, p, chip, i);
        }
    }

    int best = 0;
    for (int i = 0; i < S->n; i++) {
        int sc = cpu_score(&S->s[i]);
        if (sc > best) {
            best = sc;
            S->cpu_idx = i;
        }
    }
}

/* Readings that never change: a temperature that stayed put to the tenth
 * of a degree for 5 minutes while another moved by 10 °C or more, or an
 * input at exactly 0 V for a minute. */
static void still_update(sensors_t *S, double now)
{
    static double after = -1;
    if (after < 0) { /* test hook: HSM_STILL_AFTER=10 */
        const char *e = secure_getenv("HSM_STILL_AFTER");
        after = e && atof(e) > 0 ? atof(e) : 300;
    }
    double moved = 0;
    for (int i = 0; i < S->n; i++)
        if (S->s[i].type == SENS_TEMP && S->s[i].t_first)
            moved = MAX(moved, S->s[i].vmax - S->s[i].vmin);
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (!t->ok || !t->t_first) {
            t->still = 0;
            continue;
        }
        if (t->type == SENS_TEMP)
            t->still = t->vmax == t->vmin && now - t->t_first >= after && moved >= 10;
        else if (t->type == SENS_VOLT)
            t->still = t->vmax == 0 && t->vmin == 0 && now - t->t_first >= MIN(after, 60);
        else
            t->still = 0;
    }
}

void sensors_sample(sensors_t *S)
{
    /* how long each chip takes: a slow one (laptop firmware) gets named */
    double chip_s = 0, t = now_sec();
    S->slow_s = 0;
    S->slow[0] = '\0';
    for (int i = 0; i < S->n; i++) {
        sensor_t *s = &S->s[i];
        long long v;
        if (i && S->s[i - 1].chip_idx != s->chip_idx)
            chip_s = 0;
        if (s->hidden) { /* not even read: hiding one that is slow to answer helps */
            s->ok = 0;
            continue;
        }
        s->ok = read_ll(s->path, &v) == 0;
        double t2 = now_sec();
        chip_s += t2 - t;
        t = t2;
        if (chip_s > S->slow_s) {
            S->slow_s = chip_s;
            str_copy(S->slow, sizeof S->slow, s->chip);
        }
        if (!s->ok)
            continue;
        switch (s->type) {
        case SENS_TEMP: s->value = (double)v / 1000.0; break; /* millidegree C */
        case SENS_FAN: s->value = (double)v; break;           /* RPM */
        case SENS_POWER: s->value = (double)v / 1e6; break;   /* microwatt */
        case SENS_VOLT: s->value = (double)v / 1000.0; break; /* millivolt */
        }
        if (s->value > s->peak)
            s->peak = s->value;
        if (!s->t_first) {
            s->t_first = t;
            s->vmin = s->vmax = s->value;
        } else {
            s->vmin = MIN(s->vmin, s->value);
            s->vmax = MAX(s->vmax, s->value);
        }
    }
    still_update(S, t);
}

/* Intel coretemp exposes one "Core N" sensor per physical core; the n-th
 * coretemp chip belongs to CPU package n. AMD k10temp has no per-core
 * values, so this returns -1 there. */
double sensors_core_temp(const sensors_t *S, int pkg, int core)
{
    int pk = -1, last = -1;
    for (int i = 0; i < S->n; i++) {
        const sensor_t *s = &S->s[i];
        if (s->type != SENS_TEMP || strncmp(s->chip, "coretemp", 8))
            continue;
        if (s->chip_idx != last) {
            pk++;
            last = s->chip_idx;
        }
        int k;
        if (pk == (pkg < 0 ? 0 : pkg) && s->ok && sscanf(s->label, "Core %d", &k) == 1 && k == core)
            return s->value;
    }
    return -1;
}

double sensors_cpu_temp(const sensors_t *S)
{
    if (S->cpu_idx < 0 || !S->s[S->cpu_idx].ok)
        return -1;
    return S->s[S->cpu_idx].value;
}

/* your names, limits and hidden sensors from the config file */
void sensors_apply(sensors_t *S, const srule_t *r, int n)
{
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        t->name[0] = '\0';
        t->ulo = t->uhi = 0;
        t->hidden = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(r[k].id, t->id)) {
                str_copy(t->name, sizeof t->name, r[k].name);
                t->ulo = r[k].lo;
                t->uhi = r[k].hi;
                t->hidden = r[k].hide;
                break;
            }
        if (!t->ulo && !t->uhi) /* no limits (any more): nothing to report */
            t->lim_t = 0, t->lim_flagged = 0;
    }
}

/* a temperature no chip reaches: an input with nothing on it (127 or -128, -62 °C on boards' spare inputs) */
int sensor_plausible(const sensor_t *t)
{
    return t->type != SENS_TEMP || (t->value > -40 && t->value < 127);
}

const char *sensor_label(const sensor_t *t)
{
    return t->name[0] ? t->name : t->label;
}

int sensor_bad(const sensor_t *t)
{
    if (!t->ok)
        return 0;
    double lo = t->ulo, hi = t->uhi;
    if (t->type == SENS_VOLT) { /* your range replaces the one the chip reports */
        if (lo <= 0 && hi <= 0) {
            lo = t->lo;
            hi = t->hi;
        }
    } else if (t->type == SENS_FAN) {
        hi = 0; /* fans: only a minimum */
    } else {
        lo = 0; /* temperatures, power: only a maximum */
    }
    return (hi > 0 && t->value >= hi) || (lo > 0 && t->value < lo);
}

/* "coretemp" -> "CPU", "nvme#2" -> "NVMe 2", ... */
void sensors_chip_name(const char *chip, char *out, size_t n)
{
    static const struct {
        const char *prefix, *name;
    } map[] = { { "coretemp", "CPU" }, { "k10temp", "CPU" },   { "zenpower", "CPU" },  { "cpu_thermal", "CPU" },
                { "amdgpu", "GPU" },   { "radeon", "GPU" },    { "nouveau", "GPU" },   { "i915", "GPU" },
                { "xe", "GPU" },       { "nvme", "NVMe" },     { "acpitz", "ACPI" },   { "pch_", "PCH" },
                { "iwlwifi", "WiFi" }, { "thinkpad", "ThinkPad" }, { "drivetemp", "Drive" } };
    /* USB-C ports' power supplies: "ucsi_source_psy_USBC000:001" is port 1 */
    if (!strncmp(chip, "ucsi_source_psy", 15)) {
        const char *colon = strrchr(chip, ':');
        int port = colon ? atoi(colon + 1) : 0;
        if (port > 0)
            sfmt(out, n, "USB-C %d", port);
        else
            str_copy(out, n, "USB-C");
        return;
    }
    const char *nm = chip;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strncmp(chip, map[i].prefix, strlen(map[i].prefix))) {
            nm = map[i].name;
            break;
        }
    const char *hash = strchr(chip, '#');
    if (hash && nm != chip)
        sfmt(out, n, "%s %s", nm, hash + 1);
    else
        str_copy(out, n, nm);
}

/* "temp1", "in0", "fan2", "power1", "curr1": a number, no real name */
static int generic_label(const char *l, const char **digits)
{
    static const char *const pre[] = { "temp", "in", "fan", "power", "curr" };
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        size_t pl = strlen(pre[i]);
        if (strncmp(l, pre[i], pl) || !l[pl])
            continue;
        const char *q = l + pl;
        while (*q >= '0' && *q <= '9')
            q++;
        if (!*q) {
            *digits = l + pl;
            return 1;
        }
    }
    return 0;
}

/* A short name for the panels that tells sensors apart: your name, else
 * the chip's own label where it has one ("Core 3", "NVMe Sensor 1"), the
 * chip alone for its only reading ("WiFi", "BAT1", "USB-C 1"), or the chip
 * and a number when it has several unnamed ones ("ACPI 1", "ACPI 2"). */
void sensor_short_name(const sensors_t *S, int i, char *out, size_t n)
{
    const sensor_t *t = &S->s[i];
    if (t->name[0]) {
        str_copy(out, n, t->name);
        return;
    }
    char chip[32];
    sensors_chip_name(t->chip, chip, sizeof chip);
    if (!strncmp(t->chip, "coretemp", 8)) {
        if (!strncmp(t->label, "Package id ", 11)) {
            int k = atoi(t->label + 11);
            if (k)
                sfmt(out, n, "CPU %d", k);
            else
                str_copy(out, n, "CPU");
        } else {
            str_copy(out, n, t->label); /* "Core 3" */
        }
        return;
    }
    const char *dg = NULL;
    int comp = !strcmp(t->label, "Composite"); /* NVMe: the drive's own figure */
    if (comp || generic_label(t->label, &dg)) {
        int same = 0;
        for (int k = 0; k < S->n; k++)
            same += S->s[k].chip_idx == t->chip_idx && S->s[k].type == t->type;
        if (comp || same <= 1)
            str_copy(out, n, chip);
        else if (t->type == SENS_TEMP)
            sfmt(out, n, "%s %s", chip, dg);
        else
            sfmt(out, n, "%s %s", chip, t->label);
        return;
    }
    sfmt(out, n, "%s %s", chip, t->label);
}

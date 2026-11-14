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
/* history.c - what hsm keeps from one run to the next, in ~/.local/state/hsm:
 *
 *   events.log    every event, one per line (the Events view, 7)
 *   history.csv   one line per day: averages, peaks and totals (view 8),
 *                 to see slow changes - a machine that runs hotter every
 *                 month, a drive wearing faster, more errors than before
 *
 * Only one hsm writes them at a time (a lock), so an open screen next to a
 * --watch service doesn't count the same day twice; when the writer quits,
 * another running hsm takes over within a minute. A day's line covers the
 * hours some hsm was running ("hours"), not necessarily the whole day. */
#define _GNU_SOURCE
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EV_MAX_BYTES (4L * 1024 * 1024) /* then events.log becomes events.log.1 */
#define FLUSH_EVERY 600.0               /* seconds between saving today's line */
#define RETRY_LOCK 60.0                 /* seconds between attempts to become the writer */

static const char *const LEVEL_KEY[3] = { "info", "warn", "error" };

typedef struct {
    long long t;
    uint64_t h;
} seen_t;

static struct {
    int fd;          /* the writer lock, -1 = another hsm writes (or nothing can be written) */
    double next_try; /* monotonic */
    unsigned long seen;
    seen_t *known; /* this boot's events already in the file: the kernel log's backlog isn't written twice */
    int nknown, cknown;
    double start_wall; /* when this hsm became the writer */
    hday_t d;          /* today, so far */
    double last_mono;  /* the previous sample */
    double next_flush; /* monotonic */
    /* counters to take differences of */
    double thr_ms;
    int freezes;
    unsigned long long oom;
    int started;
} H = { .fd = -1 };

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void state_path(char *out, size_t n, const char *name)
{
    sfmt(out, n, "%s/%s", config_state_dir(), name);
}

static uint64_t fnv(const char *a, const char *b)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = a; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    h = (h ^ '|') * 1099511628211ULL;
    for (const char *p = b; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    return h;
}

/* --------------------------------------------------------- events.log */

/* "1794512790<TAB>warn<TAB>thermal<TAB>text" */
int history_parse_event(char *line, hevent_t *e)
{
    if (line[0] == '#' || !line[0])
        return -1;
    char *f[4], *save = NULL;
    int n = 0;
    for (char *p = strtok_r(line, "\t", &save); p && n < 4; p = strtok_r(NULL, "\t", &save))
        f[n++] = p;
    if (n < 4)
        return -1;
    char *end;
    e->t = strtoll(f[0], &end, 10);
    if (*end || e->t <= 0 || e->t > 4102444800LL) /* before 2100 */
        return -1;
    e->level = !strcmp(f[1], "error") ? EV_ERR : !strcmp(f[1], "warn") ? EV_WARN : EV_INFO;
    str_copy(e->cat, sizeof e->cat, f[2]);
    str_copy(e->text, sizeof e->text, f[3]);
    return 0;
}

static void remember(long long t, const char *cat, const char *text)
{
    if (H.nknown == H.cknown) {
        int nc = H.cknown ? H.cknown * 2 : 256;
        seen_t *k = realloc(H.known, (size_t)nc * sizeof *k);
        if (!k)
            return;
        H.known = k;
        H.cknown = nc;
    }
    H.known[H.nknown++] = (seen_t){ t, fnv(cat, text) };
}

/* From before this hsm started (the kernel log since boot, read again at
 * every start) and already in the file? Matched by text, not by time: the
 * time of a kernel message is worked out anew each run and can move a
 * little, or a lot when the clock was set in between. */
static int known(long long t, const char *cat, const char *text)
{
    if ((double)t >= H.start_wall)
        return 0;
    uint64_t h = fnv(cat, text);
    for (int i = 0; i < H.nknown; i++)
        if (H.known[i].h == h)
            return 1;
    return 0;
}

/* the events of this boot that are in the file already */
static void load_known(double since)
{
    H.nknown = 0;
    char p[PATHBUF];
    state_path(p, sizeof p, "events.log");
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        hevent_t e;
        if (history_parse_event(line, &e) == 0 && e.t >= (long long)since - 5)
            remember(e.t, e.cat, e.text);
    }
    fclose(f);
}

static void clean(char *dst, size_t n, const char *src) /* one line, no tabs */
{
    str_copy(dst, n, src);
    for (char *q = dst; *q; q++)
        if (*q == '\t' || *q == '\n' || *q == '\r')
            *q = ' ';
}

static void write_events(sys_t *s)
{
    const health_t *h = &s->health;
    if (h->ev_seq <= H.seen)
        return;
    char p[PATHBUF];
    state_path(p, sizeof p, "events.log");
    int who = config_as_user();
    if (who < 0)
        return;
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size > EV_MAX_BYTES) { /* keep one older file */
        char old[PATHBUF];
        state_path(old, sizeof old, "events.log.1");
        rename(p, old);
    }
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    config_as_root(who);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "a");
    if (!f) {
        close(fd);
        return;
    }
    if (fstat(fd, &st) == 0 && st.st_size == 0)
        fprintf(f, "# hsm events: time (seconds since 1970, UTC), level, category, text - tab separated\n");
    for (int k = h->nev - 1; k >= 0; k--) { /* oldest first */
        const event_t *e = health_event_at(h, k);
        if (e->seq <= H.seen)
            continue;
        char cat[16], text[256];
        clean(cat, sizeof cat, e->cat);
        clean(text, sizeof text, e->text);
        long long t = (long long)e->t;
        if (known(t, cat, text))
            continue;
        fprintf(f, "%lld\t%s\t%s\t%s\n", t, LEVEL_KEY[CLAMP(e->level, 0, 2)], cat, text);
        if (log_day_key(e->t) == H.d.day) { /* today's count */
            if (e->level == EV_ERR)
                H.d.errors++;
            else if (e->level == EV_WARN)
                H.d.warnings++;
        }
    }
    H.seen = h->ev_seq;
    fclose(f);
}

/* -------------------------------------------------------- history.csv */

static const char *const COLS =
    "date,hours,cpu_avg,cpu_max,temp_avg,temp_max,mem_avg,mem_max,swap_max_mb,power_avg_w,read_gb,written_gb,"
    "rx_gb,tx_gb,gpu_temp_max,drive_temp_max,throttled_s,freezes,oom_kills,errors,warnings,drive_wear_pct,"
    "fullest_fs_pct,battery_health_pct";

static void put_num(FILE *f, double v, int dec)
{
    if (isnan(v))
        fputs(",", f);
    else
        fprintf(f, ",%.*f", dec, v);
}

static void day_line(FILE *f, const hday_t *d)
{
    fprintf(f, "%04d-%02d-%02d", d->day / 10000, d->day / 100 % 100, d->day % 100);
    put_num(f, d->hours, 2);
    put_num(f, d->cpu_avg, 1);
    put_num(f, d->cpu_max, 1);
    put_num(f, d->temp_avg, 1);
    put_num(f, d->temp_max, 1);
    put_num(f, d->mem_avg, 1);
    put_num(f, d->mem_max, 1);
    put_num(f, d->swap_max, 0);
    put_num(f, d->power_avg, 1);
    put_num(f, d->read_gb, 3);
    put_num(f, d->written_gb, 3);
    put_num(f, d->rx_gb, 3);
    put_num(f, d->tx_gb, 3);
    put_num(f, d->gpu_temp_max, 1);
    put_num(f, d->drive_temp_max, 1);
    put_num(f, d->throttled_s, 1);
    fprintf(f, ",%d,%d,%d,%d", d->freezes, d->oom, d->errors, d->warnings);
    put_num(f, d->wear, 0);
    put_num(f, d->fullest, 1);
    put_num(f, d->battery, 1);
    fputc('\n', f);
}

static double fld(char **f, int n, int i)
{
    if (i >= n || !f[i][0])
        return NAN;
    char *end;
    double v = strtod(f[i], &end);
    return end == f[i] || !isfinite(v) || fabs(v) > 1e12 ? NAN : v;
}

static int ifld(char **f, int n, int i) /* a count: 0 when missing or nonsense */
{
    double v = fld(f, n, i);
    return isnan(v) || v < 0 || v > 1e9 ? 0 : (int)v;
}

int history_parse_day(char *line, hday_t *d)
{
    char *f[32];
    int n = 0;
    for (char *p = line; n < 32;) { /* empty fields count: no strtok */
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c)
            break;
        *c = '\0';
        p = c + 1;
    }
    int y, m, dd;
    if (n < 24 || sscanf(f[0], "%4d-%2d-%2d", &y, &m, &dd) != 3 || y < 1970 || y > 9999 || m < 1 || m > 12 || dd < 1 ||
        dd > 31)
        return -1;
    memset(d, 0, sizeof *d);
    d->day = y * 10000 + m * 100 + dd;
    d->hours = fld(f, n, 1);
    d->cpu_avg = fld(f, n, 2);
    d->cpu_max = fld(f, n, 3);
    d->temp_avg = fld(f, n, 4);
    d->temp_max = fld(f, n, 5);
    d->mem_avg = fld(f, n, 6);
    d->mem_max = fld(f, n, 7);
    d->swap_max = fld(f, n, 8);
    d->power_avg = fld(f, n, 9);
    d->read_gb = fld(f, n, 10);
    d->written_gb = fld(f, n, 11);
    d->rx_gb = fld(f, n, 12);
    d->tx_gb = fld(f, n, 13);
    d->gpu_temp_max = fld(f, n, 14);
    d->drive_temp_max = fld(f, n, 15);
    d->throttled_s = fld(f, n, 16);
    d->freezes = ifld(f, n, 17);
    d->oom = ifld(f, n, 18);
    d->errors = ifld(f, n, 19);
    d->warnings = ifld(f, n, 20);
    d->wear = fld(f, n, 21);
    d->fullest = fld(f, n, 22);
    d->battery = fld(f, n, 23);
    if (isnan(d->hours) || d->hours < 0 || d->hours > 24.5)
        d->hours = 0;
    return 0;
}

/* averages are kept as sums while the day runs; the line has averages */
static void finish(const hday_t *acc, hday_t *out)
{
    *out = *acc;
    double h = acc->hours > 0 ? acc->hours : 1;
    out->cpu_avg = acc->cpu_avg / h;
    out->mem_avg = acc->mem_avg / h;
    out->temp_avg = acc->temp_h > 0 ? acc->temp_avg / acc->temp_h : NAN;
    out->power_avg = acc->power_h > 0 ? acc->power_avg / acc->power_h : NAN;
}

static void nan_max(double *a, double b)
{
    if (!isnan(b) && (isnan(*a) || b > *a))
        *a = b;
}

static void empty_day(hday_t *d, int day)
{
    memset(d, 0, sizeof *d);
    d->day = day;
    d->cpu_max = d->mem_max = d->temp_max = d->swap_max = NAN;
    d->gpu_temp_max = d->drive_temp_max = d->wear = d->fullest = d->battery = NAN;
}

/* rewrite history.csv with today's line replaced (a few hundred lines at most) */
static void flush_day(void)
{
    if (H.fd < 0 || H.d.hours <= 0)
        return;
    char p[PATHBUF], tmp[PATHBUF + 8];
    state_path(p, sizeof p, "history.csv");
    sfmt(tmp, sizeof tmp, "%s.tmp", p);
    hday_t out;
    finish(&H.d, &out);
    int who = config_as_user();
    if (who < 0)
        return;
    FILE *in = fopen(p, "r");
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        if (in)
            fclose(in);
        config_as_root(who);
        return;
    }
    fprintf(f, "%s\n", COLS);
    int put = 0;
    char line[1024];
    while (in && fgets(line, sizeof line, in)) {
        if (!strncmp(line, "date,", 5))
            continue;
        char copy[1024];
        str_copy(copy, sizeof copy, line);
        hday_t d;
        if (history_parse_day(copy, &d) != 0)
            continue;
        if (d.day == out.day) {
            day_line(f, &out);
            put = 1;
        } else {
            if (!put && d.day > out.day) { /* keep the dates in order */
                day_line(f, &out);
                put = 1;
            }
            fputs(line, f);
            if (line[strlen(line) - 1] != '\n')
                fputc('\n', f);
        }
    }
    if (!put)
        day_line(f, &out);
    if (in)
        fclose(in);
    if (fclose(f) == 0)
        rename(tmp, p);
    else
        unlink(tmp);
    config_as_root(who);
}

/* an earlier run today: go on from its figures */
static void load_today(int day)
{
    empty_day(&H.d, day);
    char p[PATHBUF];
    state_path(p, sizeof p, "history.csv");
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[1024];
    hday_t d;
    while (fgets(line, sizeof line, f))
        if (history_parse_day(line, &d) == 0 && d.day == day && d.hours > 0) {
            H.d = d; /* averages back into sums */
            H.d.cpu_avg = isnan(d.cpu_avg) ? 0 : d.cpu_avg * d.hours;
            H.d.mem_avg = isnan(d.mem_avg) ? 0 : d.mem_avg * d.hours;
            H.d.temp_h = isnan(d.temp_avg) ? 0 : d.hours;
            H.d.temp_avg = isnan(d.temp_avg) ? 0 : d.temp_avg * d.hours;
            H.d.power_h = isnan(d.power_avg) ? 0 : d.hours;
            H.d.power_avg = isnan(d.power_avg) ? 0 : d.power_avg * d.hours;
            if (isnan(H.d.read_gb))
                H.d.read_gb = 0;
            if (isnan(H.d.written_gb))
                H.d.written_gb = 0;
            if (isnan(H.d.rx_gb))
                H.d.rx_gb = 0;
            if (isnan(H.d.tx_gb))
                H.d.tx_gb = 0;
            if (isnan(H.d.throttled_s))
                H.d.throttled_s = 0;
        }
    fclose(f);
}

static void sample_day(sys_t *s, double dt)
{
    hday_t *d = &H.d;
    double hr = dt / 3600.0;
    d->hours += hr;
    d->cpu_avg += s->cpu.total * hr;
    nan_max(&d->cpu_max, s->cpu.total);
    if (s->cpu.temp_c >= 0) {
        d->temp_avg += s->cpu.temp_c * hr;
        d->temp_h += hr;
        nan_max(&d->temp_max, s->cpu.temp_c);
    }
    double mem = s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : NAN;
    if (!isnan(mem)) {
        d->mem_avg += mem * hr;
        nan_max(&d->mem_max, mem);
    }
    if (s->mem.swap_total)
        nan_max(&d->swap_max, (double)s->mem.swap_used / (1024.0 * 1024.0));
    if (s->total_w >= 0) {
        d->power_avg += s->total_w * hr;
        d->power_h += hr;
    }
    const double GB = 1024.0 * 1024.0 * 1024.0;
    d->read_gb += s->disks.rd_bps * dt / GB;
    d->written_gb += s->disks.wr_bps * dt / GB;
    d->rx_gb += s->net.rx_bps * dt / GB;
    d->tx_gb += s->net.tx_bps * dt / GB;
    for (int i = 0; i < s->gpus.n; i++)
        if (s->gpus.g[i].temp_c >= 0)
            nan_max(&d->gpu_temp_max, s->gpus.g[i].temp_c);
    const health_t *h = &s->health;
    for (int i = 0; i < h->nnvme; i++)
        if (h->nvme[i].state == 1) {
            nan_max(&d->drive_temp_max, h->nvme[i].temp_c);
            d->wear = isnan(d->wear) ? h->nvme[i].used : MAX(d->wear, h->nvme[i].used);
        }
    int smart_temp = 0;
    for (int i = 0; i < h->nnvme; i++)
        smart_temp |= h->nvme[i].state == 1;
    for (int i = 0; i < h->nsata; i++)
        if (h->sata[i].state == 1) {
            if (h->sata[i].temp_c > 0) {
                nan_max(&d->drive_temp_max, h->sata[i].temp_c);
                smart_temp = 1;
            }
            if (h->sata[i].wear >= 0)
                d->wear = isnan(d->wear) ? h->sata[i].wear : MAX(d->wear, h->sata[i].wear);
        }
    /* without root no drive health, but the drives' own sensors are there to
     * read: NVMe "Composite" (what SMART reports too) and drivetemp */
    if (!smart_temp)
        for (int i = 0; i < s->sens.n; i++) {
            const sensor_t *t = &s->sens.s[i];
            if (t->type != SENS_TEMP || !t->ok)
                continue;
            if ((!strncmp(t->chip, "nvme", 4) && !strcmp(t->label, "Composite")) || !strncmp(t->chip, "drivetemp", 9))
                nan_max(&d->drive_temp_max, t->value);
        }
    /* counters: what was added since the last sample */
    if (h->thr_ok) {
        if (h->thr_ms > H.thr_ms && H.thr_ms > 0)
            d->throttled_s += (h->thr_ms - H.thr_ms) / 1000.0;
        H.thr_ms = h->thr_ms;
    }
    if (h->freezes > H.freezes)
        d->freezes += h->freezes - H.freezes;
    H.freezes = h->freezes;
    if (s->mem.oom_ok) {
        if (s->mem.oom_kill > H.oom && H.oom > 0)
            d->oom += (int)(s->mem.oom_kill - H.oom);
        H.oom = s->mem.oom_kill;
    }
    /* "now" values: the latest of the day */
    double full = NAN;
    for (int i = 0; i < s->disks.nfs; i++) {
        const fs_t *f = &s->disks.fs[i];
        if (!f->readonly && f->used + f->avail)
            nan_max(&full, 100.0 * (double)f->used / (double)(f->used + f->avail));
    }
    if (!isnan(full))
        d->fullest = full;
    for (int i = 0; i < s->power.nbat; i++)
        if (s->power.bat[i].health > 0)
            d->battery = s->power.bat[i].health;
}

/* ------------------------------------------------------------ the writer */

static int try_lock(void)
{
    char p[PATHBUF];
    state_path(p, sizeof p, "writer.lock");
    int who = config_as_user();
    if (who < 0)
        return -1;
    int fd = -1;
    if (config_mkdir_p(config_state_dir()) == 0)
        fd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    config_as_root(who);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { /* another hsm writes */
        close(fd);
        return -1;
    }
    return fd;
}

static void become_writer(sys_t *s)
{
    H.fd = try_lock();
    if (H.fd < 0)
        return;
    double boot = wall() - s->cpu.uptime;
    H.start_wall = wall();
    load_known(boot);
    H.seen = 0; /* everything in memory, the kernel log's backlog too; the known ones are skipped */
    load_today(log_day_key(wall()));
    prog_day_start(log_day_key(wall()));
    H.next_flush = now_sec() + FLUSH_EVERY;
}

void history_start(sys_t *s)
{
    if (H.started)
        return;
    H.started = 1;
    H.last_mono = now_sec();
    H.thr_ms = s->health.thr_ms;
    H.freezes = s->health.freezes;
    H.oom = s->mem.oom_ok ? s->mem.oom_kill : 0;
    become_writer(s);
    H.next_try = now_sec() + RETRY_LOCK;
}

void history_tick(sys_t *s)
{
    if (!H.started)
        return;
    double mono = now_sec(), dt = mono - H.last_mono;
    H.last_mono = mono;
    if (H.fd < 0) {
        if (mono < H.next_try)
            return;
        H.next_try = mono + RETRY_LOCK;
        become_writer(s);
        if (H.fd < 0)
            return;
        H.thr_ms = s->health.thr_ms; /* counting starts now */
        H.freezes = s->health.freezes;
        H.oom = s->mem.oom_ok ? s->mem.oom_kill : 0;
        dt = 0;
    }
    int today = log_day_key(wall());
    if (today != H.d.day) { /* midnight: the old day is done */
        flush_day();
        prog_flush();
        empty_day(&H.d, today);
        prog_day_start(today);
    }
    write_events(s);
    if (dt > 0 && dt < 30) { /* longer: suspended or paused, not watched */
        sample_day(s, dt);
        prog_sample(s, dt);
    }
    if (mono >= H.next_flush) {
        H.next_flush = mono + FLUSH_EVERY;
        flush_day();
        prog_flush();
    }
}

void history_end(sys_t *s)
{
    if (H.fd < 0)
        return;
    write_events(s); /* e.g. a check stopped by quitting, a moment ago */
    flush_day();
    prog_flush();
    close(H.fd); /* the lock goes with it */
    H.fd = -1;
    free(H.known);
    H.known = NULL;
    H.nknown = H.cknown = 0;
}

int history_writer(void)
{
    return H.fd >= 0;
}

int history_today(hday_t *out)
{
    if (H.fd < 0 || H.d.hours <= 0)
        return -1;
    finish(&H.d, out);
    return 0;
}

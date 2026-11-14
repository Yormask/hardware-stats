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
/* health.c - signs of trouble: thermal throttling, pressure stalls, out-of-
 * memory kills, PCIe bus errors, NVMe drive health, the kernel's own error
 * messages, freezes and suspends, fans stopping, network links dropping,
 * file systems turning read-only and sensors passing your limits, plus a
 * list of "events" that collects all of these with a time stamp, for the
 * screen and for the logs. */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/nvme_ioctl.h>)
#include <linux/nvme_ioctl.h>
#define HAVE_NVME_IOCTL 1
#endif
#endif

#define NVME_EVERY 60.0 /* seconds between drive health reads */
#define HEAT_QUIET 300.0 /* s with no new heat slowdown: the drive's run of them is over */

static double wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void app_s(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void app_s(char *b, size_t n, const char *fmt, ...)
{
    size_t l = strlen(b);
    if (l + 1 >= n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + l, n - l, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- events */

static void ev_add(health_t *h, double t, int level, const char *cat, const char *text)
{
    /* The same line again right after the last one (the kernel retries and
     * logs every try): counted on that one, "... (3 times)", not a new event. */
    if (h->nev) {
        event_t *l = &h->ev[(h->ev_head - 1 + MAX_EVENTS) % MAX_EVENTS];
        size_t tl = strlen(text);
        char suf[24] = "";
        if (l->rep > 1)
            sfmt(suf, sizeof suf, " (%d times)", l->rep);
        if (l->level == level && !strcmp(l->cat, cat) && t >= l->t - 1 && t - l->t_rep <= 5 &&
            tl < sizeof l->text - 16 && !strncmp(l->text, text, tl) && !strcmp(l->text + tl, suf)) {
            char b[200];
            l->rep = l->rep > 1 ? l->rep + 1 : 2;
            l->t_rep = t;
            sfmt(b, sizeof b, "%s (%d times)", text, l->rep);
            str_copy(l->text, sizeof l->text, b);
            return;
        }
    }
    event_t *e = &h->ev[h->ev_head];
    h->ev_head = (h->ev_head + 1) % MAX_EVENTS;
    if (h->nev < MAX_EVENTS)
        h->nev++;
    e->t = e->t_rep = t;
    e->rep = 1;
    e->seq = ++h->ev_seq;
    e->level = level;
    str_copy(e->cat, sizeof e->cat, cat);
    str_copy(e->text, sizeof e->text, text);
}

void health_event(health_t *h, int level, const char *cat, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ev_add(h, wall_now(), level, cat, b);
}

const event_t *health_event_at(const health_t *h, int i)
{
    if (i < 0 || i >= h->nev)
        return NULL;
    return &h->ev[(h->ev_head - 1 - i + 2 * MAX_EVENTS) % MAX_EVENTS];
}

int health_alert(const health_t *h, char *out, size_t n)
{
    double now = wall_now();
    const char *what[16];
    int k = 0, lvl = 0;
    if (h->thr_last > 0 && now - h->thr_last < 10) {
        what[k++] = "throttling";
        lvl = 2;
    }
    if (h->hot_last > 0 && now - h->hot_last < 10) {
        what[k++] = "too hot";
        lvl = 2;
    }
    if (h->fan_last > 0 && now - h->fan_last < 10) {
        what[k++] = "fan stopped";
        lvl = 2;
    }
    if (h->oom_last > 0 && now - h->oom_last < 60) {
        what[k++] = "out of memory";
        lvl = 2;
    }
    if (h->freeze_t > 0 && now - h->freeze_t < 60) {
        what[k++] = "system froze";
        lvl = MAX(lvl, h->freeze_last >= 5 ? 2 : 1);
    }
    int drive = 0;
    for (int i = 0; i < h->nnvme; i++)
        drive |= h->nvme[i].state == 1 && h->nvme[i].crit;
    for (int i = 0; i < h->nsata; i++) /* failing, or sectors it can't read */
        drive |= h->sata[i].state == 1 && (h->sata[i].failing || h->sata[i].pending > 0 || h->sata[i].uncorrect > 0);
    if (drive) {
        what[k++] = "drive warning";
        lvl = 2;
    }
    if (h->ro_last > 0 && now - h->ro_last < 10) {
        what[k++] = "disk read-only";
        lvl = 2;
    }
    if (h->lowmem_last > 0 && now - h->lowmem_last < 10) {
        what[k++] = "low memory";
        lvl = MAX(lvl, 1);
    }
    if (h->full_last > 0 && now - h->full_last < 10) {
        what[k++] = "disk almost full";
        lvl = MAX(lvl, 1);
    }
    if (h->ulim_last > 0 && now - h->ulim_last < 10) {
        what[k++] = "past your limit";
        lvl = MAX(lvl, 1);
    }
    if (h->slow_last > 0 && now - h->slow_last < 10) {
        what[k++] = "disk slow";
        lvl = MAX(lvl, 1);
    }
    if (h->link_last > 0 && now - h->link_last < 60) {
        what[k++] = "link dropped";
        lvl = MAX(lvl, 1);
    }
    if (h->pci_last > 0 && now - h->pci_last < 60) {
        what[k++] = "PCIe errors";
        lvl = MAX(lvl, 1);
    }
    if (h->kmsg_last > 0 && now - h->kmsg_last < 60) {
        what[k++] = "kernel error";
        lvl = MAX(lvl, 1);
    }
    if (!k) {
        out[0] = '\0';
        return 0;
    }
    if (k == 1)
        str_copy(out, n, what[0]);
    else
        sfmt(out, n, "%s +%d", what[0], k - 1);
    return lvl;
}

/* ------------------------------------------------- pressure stall info */

static void psi_sample(psi_t *p, const char *name, double dt)
{
    char path[PATHBUF], buf[512];
    sfmt(path, sizeof path, "%s/pressure/%s", g_procfs, name);
    if (read_file(path, buf, sizeof buf) <= 0) { /* Linux < 4.20, or booted with psi=0 */
        p->ok = 0;
        return;
    }
    p->ok = 1;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        double a10, a60, a300;
        unsigned long long tot;
        int full = !strncmp(line, "full", 4);
        if (sscanf(line + 4, " avg10=%lf avg60=%lf avg300=%lf total=%llu", &a10, &a60, &a300, &tot) != 4)
            continue;
        double pct = 0;
        unsigned long long *prev = full ? &p->p_full : &p->p_some;
        if (p->primed && dt > 0 && tot >= *prev)
            pct = CLAMP((double)(tot - *prev) / (dt * 1e6) * 100.0, 0, 100); /* total is in us */
        *prev = tot;
        if (full) {
            p->has_full = 1;
            p->full = pct;
            p->favg10 = a10;
            p->favg60 = a60;
            p->favg300 = a300;
        } else {
            p->some = pct;
            p->avg10 = a10;
            p->avg60 = a60;
            p->avg300 = a300;
        }
    }
    p->primed = 1;
}

/* ------------------------------------------------------ thermal throttling */

/* Intel CPUs count every time a core or the whole package had to slow down
 * because it reached its temperature limit (PROCHOT), and since Linux 5.5
 * also for how long. The counters live under each CPU's thermal_throttle/. */
static int thr_read(int cpu, const char *what, unsigned long long *out)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/thermal_throttle/%s", g_sysfs, cpu, what);
    return read_ull(p, out);
}

/* ------------------------------------------------------------- blame */

void health_blame(const procs_t *ps, int what, char *out, size_t n)
{
    out[0] = '\0';
    const proc_t *best = NULL;
    double bv = 0;
    for (int i = 0; i < ps->n; i++) {
        const proc_t *p = &ps->p[i];
        double v = what == BLAME_CPU ? p->cpu
                 : what == BLAME_MEM ? (double)p->rss
                 : what == BLAME_IO  ? (p->io_ok ? p->rd_bps + p->wr_bps : 0)
                                     : p->gpu;
        if (v > bv) {
            bv = v;
            best = p;
        }
    }
    if (!best)
        return;
    char b[16];
    switch (what) {
    case BLAME_CPU:
        if (bv >= 15)
            sfmt(out, n, " while %s used %.0f%% CPU", best->comm, bv);
        break;
    case BLAME_MEM:
        fmt_bytes(bv, b, sizeof b);
        sfmt(out, n, " (biggest: %s, %s)", best->comm, b);
        break;
    case BLAME_IO:
        if (bv >= 1e6) {
            fmt_bytes(MAX(best->rd_bps, best->wr_bps), b, sizeof b);
            sfmt(out, n, " while %s %s %s/s", best->comm, best->wr_bps > best->rd_bps ? "wrote" : "read", b);
        }
        break;
    case BLAME_GPU:
        if (bv >= 15)
            sfmt(out, n, " while %s used %.0f%% GPU", best->comm, bv);
        break;
    }
}

static void throttle_sample(health_t *h, const cpu_t *c, const procs_t *ps, double dt, double now)
{
    if (!c->ncpu)
        return;
    char p[PATHBUF];
    if (!h->thr_tried) {
        sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/thermal_throttle", g_sysfs, c->id[0]);
        h->thr_ok = path_exists(p);
        h->thr_tried = 1;
    }
    if (!h->thr_ok)
        return;
    int first = !h->thr_start && !h->thr_count && !h->core_count; /* baseline on the first read */

    /* packages */
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i], pkg = c->pkg_id[id] >= 0 ? c->pkg_id[id] : 0, k;
        for (k = 0; k < h->nthr && h->thr[k].pkg != pkg; k++)
            ;
        if (k == h->nthr && h->nthr < MAX_PKGS) {
            memset(&h->thr[k], 0, sizeof h->thr[k]);
            h->thr[k].pkg = pkg;
            h->thr[k].cpu = id;
            h->thr[k].p_count = (unsigned long long)-1;
            h->nthr++;
        }
    }
    int events = 0;
    double worst = 0, ms = 0;
    unsigned long long cnt = 0, ccnt = 0;
    for (int k = 0; k < h->nthr; k++) {
        thr_pkg_t *t = &h->thr[k];
        unsigned long long n = 0, m = 0;
        thr_read(t->cpu, "package_throttle_count", &n);
        int has_ms = thr_read(t->cpu, "package_throttle_total_time_ms", &m) == 0;
        t->pct = 0;
        if (t->p_count != (unsigned long long)-1 && dt > 0) {
            if (n > t->p_count)
                events++;
            if (has_ms && m >= t->p_ms)
                t->pct = CLAMP((double)(m - t->p_ms) / (dt * 10.0), 0, 100);
            else if (n > t->p_count)
                t->pct = 100; /* old kernels: no time, just "it happened" */
        }
        t->count = n;
        t->ms = m;
        t->p_count = n;
        t->p_ms = m;
        cnt += n;
        ms += (double)m;
        worst = MAX(worst, t->pct);
    }
    /* cores: one CPU per physical core (hyper-threads share the counters) */
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i], dup = 0;
        if (id < 0 || id >= MAX_CPUS)
            continue;
        for (int j = 0; j < i && !dup; j++) {
            int o = c->id[j];
            dup = o >= 0 && o < MAX_CPUS && c->core_id[o] == c->core_id[id] && c->pkg_id[o] == c->pkg_id[id] &&
                  c->core_id[id] >= 0;
        }
        if (dup)
            continue;
        unsigned long long n = 0, m = 0;
        if (thr_read(id, "core_throttle_count", &n))
            continue;
        int has_ms = thr_read(id, "core_throttle_total_time_ms", &m) == 0;
        if (!first && dt > 0) {
            if (n > h->core_cnt[id])
                events++;
            if (has_ms && m >= h->core_ms[id])
                worst = MAX(worst, CLAMP((double)(m - h->core_ms[id]) / (dt * 10.0), 0, 100));
        }
        h->core_cnt[id] = n;
        h->core_ms[id] = m;
        ccnt += n;
    }
    h->thr_count = cnt;
    h->core_count = ccnt;
    h->thr_ms = ms;
    h->thr_pct = worst;
    if (first)
        h->thr_start = cnt + ccnt;
    if (events) {
        h->thr_last = now;
        if (now - h->thr_ev_t >= 60) {
            char who[80];
            h->thr_ev_t = now;
            health_blame(ps, BLAME_CPU, who, sizeof who);
            if (c->temp_c >= 0)
                health_event(h, EV_WARN, "thermal", "CPU throttled (%.0f °C)%s", c->temp_c, who);
            else
                health_event(h, EV_WARN, "thermal", "CPU throttled by heat%s", who);
        }
    }
}

/* ------------------------------------------------------------ PCIe (AER) */

static unsigned long long aer_total(const char *dev, const char *file, const char *key, int *ok)
{
    char p[PATHBUF], buf[2048];
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/%s", g_sysfs, dev, file);
    if (read_file(p, buf, sizeof buf) <= 0)
        return 0;
    *ok = 1;
    char *k = strstr(buf, key);
    return k ? strtoull(k + strlen(key), NULL, 10) : 0;
}

static void pci_name(const char *dev, char *out, size_t n)
{
    char p[PATHBUF];
    unsigned ven = 0, did = 0;
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/vendor", g_sysfs, dev);
    read_hex(p, &ven);
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/device", g_sysfs, dev);
    read_hex(p, &did);
    if (pci_lookup_name(ven, did, out, n))
        sfmt(out, n, "PCI device %04x:%04x", ven, did);
}

static void aer_sample(health_t *h, double now)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/bus/pci/devices", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    unsigned long long tc = 0, tn = 0, tf = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int ok = 0;
        unsigned long long c = aer_total(e->d_name, "aer_dev_correctable", "TOTAL_ERR_COR ", &ok);
        unsigned long long nf = aer_total(e->d_name, "aer_dev_nonfatal", "TOTAL_ERR_NONFATAL ", &ok);
        unsigned long long f = aer_total(e->d_name, "aer_dev_fatal", "TOTAL_ERR_FATAL ", &ok);
        if (!ok)
            continue;
        h->aer_ok = 1;
        tc += c;
        tn += nf;
        tf += f;
        if (!c && !nf && !f)
            continue;
        int k;
        for (k = 0; k < h->npci && strcmp(h->pci[k].dev, e->d_name); k++)
            ;
        if (k == h->npci) {
            if (h->npci >= MAX_PCIDEV)
                continue;
            pcidev_t *pd = &h->pci[h->npci++];
            memset(pd, 0, sizeof *pd);
            str_copy(pd->dev, sizeof pd->dev, e->d_name);
            pci_name(e->d_name, pd->name, sizeof pd->name);
            if (h->primed) { /* the device had none before */
                pd->cor = pd->nonfatal = pd->fatal = 0;
            } else {
                pd->cor = c;
                pd->nonfatal = nf;
                pd->fatal = f;
            }
        }
        pcidev_t *pd = &h->pci[k];
        if (c > pd->cor || nf > pd->nonfatal || f > pd->fatal) {
            int bad = nf > pd->nonfatal || f > pd->fatal;
            health_event(h, bad ? EV_ERR : EV_WARN, "pcie", "PCIe %s: %llu new %s error%s (%s)", pd->dev,
                         bad ? (nf - pd->nonfatal) + (f - pd->fatal) : c - pd->cor, bad ? "uncorrected" : "corrected",
                         (bad ? (nf - pd->nonfatal) + (f - pd->fatal) : c - pd->cor) == 1 ? "" : "s", pd->name);
            h->pci_last = now;
        }
        pd->cor = c;
        pd->nonfatal = nf;
        pd->fatal = f;
    }
    closedir(d);
    h->pci_cor = tc;
    h->pci_nonfatal = tn;
    h->pci_fatal = tf;
}

/* ---------------------------------------------------------- NVMe health */

#ifndef HAVE_NVME_IOCTL /* the kernel ABI, for systems without the header */
struct nvme_admin_cmd {
    uint8_t opcode, flags;
    uint16_t rsvd1;
    uint32_t nsid, cdw2, cdw3;
    uint64_t metadata, addr;
    uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
    uint32_t timeout_ms, result;
};
#define NVME_IOCTL_ADMIN_CMD _IOWR('N', 0x41, struct nvme_admin_cmd)
#endif

static unsigned long long le(const unsigned char *b, int n)
{
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

/* "SMART / Health Information" log page (NVMe spec, Get Log Page 02h): a
 * read-only query the drive answers from its own counters. */
static int nvme_read_log(const char *name, unsigned char *buf, int *state)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s", g_devfs, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *state = (errno == EACCES || errno == EPERM) ? -1 : -2;
        return -1;
    }
    struct stat st;
    int r = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) { /* test hook: a captured log page */
        r = read(fd, buf, 512) == 512 ? 0 : -1;
    } else if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
        struct nvme_admin_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = 0x02; /* Get Log Page */
        cmd.nsid = 0xffffffffu;
        cmd.addr = (uint64_t)(uintptr_t)buf;
        cmd.data_len = 512;
        cmd.cdw10 = (127u << 16) | 0x02; /* 128 dwords of log page 02h */
        r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0 ? 0 : -1;
        if (r)
            *state = (errno == EACCES || errno == EPERM) ? -1 : -2;
    }
    close(fd);
    if (!r)
        *state = 1;
    else if (*state == 1 || *state == 0)
        *state = -2;
    return r;
}

static void nvme_find(health_t *h)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/class/nvme", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && h->nnvme < MAX_NVME) {
        if (strncmp(e->d_name, "nvme", 4) || !e->d_name[4])
            continue;
        nvme_t *v = &h->nvme[h->nnvme++];
        memset(v, 0, sizeof *v);
        str_copy(v->name, sizeof v->name, e->d_name);
        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%s/model", base, e->d_name);
        if (read_line(p, v->model, sizeof v->model))
            v->model[0] = '\0';
    }
    closedir(d);
    for (int i = 1; i < h->nnvme; i++) /* nvme0, nvme1, ... nvme10 */
        for (int k = i; k > 0 && atoi(h->nvme[k - 1].name + 4) > atoi(h->nvme[k].name + 4); k--) {
            nvme_t t = h->nvme[k - 1];
            h->nvme[k - 1] = h->nvme[k];
            h->nvme[k] = t;
        }
}

/* The drive counts its own heat slowdowns (thermal management): a long copy
 * or a game install on a hot laptop shows up here. One event when it starts,
 * one if it gets to the hard kind, one when it's over. */
static void nvme_heat(health_t *h, const procs_t *ps, nvme_t *v, const unsigned long long n0[2],
                      const unsigned long long s0[2])
{
    double now = wall_now();
    int up[2];
    for (int k = 0; k < 2; k++)
        up[k] = v->heat_n[k] > n0[k] || v->heat_s[k] > s0[k];
    char tc[24] = "", who[80];
    if (v->temp_c >= 0)
        sfmt(tc, sizeof tc, " (%.0f °C)", v->temp_c);
    if (up[0] || up[1]) {
        if (!v->heat_open) {
            v->heat_open = 1;
            v->heat_hard = up[1];
            v->heat_from = now;
            v->heat_max_c = v->temp_c;
            memcpy(v->heat_n0, n0, sizeof v->heat_n0);
            memcpy(v->heat_s0, s0, sizeof v->heat_s0);
            health_blame(ps, BLAME_IO, who, sizeof who);
            health_event(h, up[1] ? EV_WARN : EV_INFO, "disk", "%s slowed itself down %s to cool off%s%s", v->name,
                         up[1] ? "hard" : "a little", tc, who);
        } else if (up[1] && !v->heat_hard) {
            v->heat_hard = 1;
            health_blame(ps, BLAME_IO, who, sizeof who);
            health_event(h, EV_WARN, "disk", "%s now slows itself down hard to cool off%s%s", v->name, tc, who);
        }
        v->heat_t = now;
        v->heat_max_c = MAX(v->heat_max_c, v->temp_c);
    } else if (v->heat_open && now - v->heat_t >= HEAT_QUIET) {
        v->heat_open = 0;
        unsigned long long s[2], n = 0;
        for (int k = 0; k < 2; k++) {
            s[k] = v->heat_s[k] > v->heat_s0[k] ? v->heat_s[k] - v->heat_s0[k] : 0;
            n += v->heat_n[k] > v->heat_n0[k] ? v->heat_n[k] - v->heat_n0[k] : 0;
        }
        char how[48], hard[40] = "", in[24], top[24] = "";
        if (s[0] + s[1]) {
            char d[24];
            fmt_duration((double)(s[0] + s[1]), d, sizeof d);
            str_copy(how, sizeof how, d);
            if (s[1]) {
                fmt_duration((double)s[1], d, sizeof d);
                sfmt(hard, sizeof hard, " (%s of it hard)", d);
            }
        } else {
            sfmt(how, sizeof how, "%llu time%s", n, n == 1 ? "" : "s");
        }
        fmt_duration(v->heat_t - v->heat_from + NVME_EVERY, in, sizeof in); /* it began in the minute before */
        if (v->heat_max_c >= 0)
            sfmt(top, sizeof top, ", up to %.0f °C", v->heat_max_c);
        health_event(h, EV_INFO, "disk", "%s heat slowdowns over: slowed %s%s within %s%s", v->name, how, hard, in, top);
    }
}

static void nvme_sample(health_t *h, const procs_t *ps)
{
    static const char *const warn[] = { "spare space below threshold", "temperature outside its limits",
                                        "reliability degraded", "drive switched to read-only",
                                        "backup power failed", "memory region read-only" };
    for (int i = 0; i < h->nnvme; i++) {
        nvme_t *v = &h->nvme[i];
        unsigned char b[512];
        int was = v->state;
        unsigned old_crit = v->crit;
        unsigned long long old_media = v->media_err, n0[2] = { v->heat_n[0], v->heat_n[1] },
                           s0[2] = { v->heat_s[0], v->heat_s[1] };
        if (nvme_read_log(v->name, b, &v->state))
            continue;
        v->crit = b[0];
        unsigned k = (unsigned)le(b + 1, 2);
        v->temp_c = k > 0 ? (double)k - 273.15 : -1;
        v->spare = b[3];
        v->spare_thr = b[4];
        v->used = b[5];
        v->read_b = (double)le(b + 32, 8) * 512000.0; /* units of 1000 x 512 bytes */
        v->written_b = (double)le(b + 48, 8) * 512000.0;
        v->cycles = le(b + 112, 8);
        v->hours = le(b + 128, 8);
        v->unsafe = le(b + 144, 8);
        v->media_err = le(b + 160, 8);
        v->err_log = le(b + 176, 8);
        v->warn_min = le(b + 192, 4);
        v->crit_min = le(b + 196, 4);
        for (int t = 0; t < 2; t++) { /* thermal management: transitions, then total seconds */
            v->heat_n[t] = le(b + 216 + 4 * t, 4);
            v->heat_s[t] = le(b + 224 + 4 * t, 4);
        }
        if (was == 1)
            nvme_heat(h, ps, v, n0, s0);
        for (int bit = 0; bit < 6; bit++)
            if ((v->crit & (1u << bit)) && (was != 1 || !(old_crit & (1u << bit))))
                health_event(h, EV_ERR, "disk", "%s: %s", v->name, warn[bit]);
        if (was == 1 && v->media_err > old_media)
            health_event(h, EV_ERR, "disk", "%s: %llu new media / data integrity error%s", v->name,
                         v->media_err - old_media, v->media_err - old_media == 1 ? "" : "s");
    }
}

/* ------------------------------------------------------------ kernel log */

/* Which kernel messages matter here. Matched anywhere in the text, at any
 * log level; anything at "error" level or worse is kept as well. */
static const struct {
    const char *needle, *cat;
    int level;
} KMSG[] = {
    { "temperature above threshold", "thermal", EV_WARN },
    { "clock throttled", "thermal", EV_WARN },
    { "critical temperature", "thermal", EV_ERR },
    { "thermal shutdown", "thermal", EV_ERR },
    { "Thermal shutdown", "thermal", EV_ERR },
    { "Out of memory", "memory", EV_ERR },
    { "invoked oom-killer", "memory", EV_ERR },
    { "Killed process", "memory", EV_ERR },
    { "Memory failure", "memory", EV_ERR },
    { "Hardware Error", "memory", EV_ERR },
    { "Machine check", "memory", EV_ERR },
    { "EDAC", "memory", EV_WARN },
    { "I/O error", "disk", EV_ERR },
    { "critical medium error", "disk", EV_ERR },
    { "Medium Error", "disk", EV_ERR },
    { "EXT4-fs error", "disk", EV_ERR },
    { "BTRFS error", "disk", EV_ERR },
    { "BTRFS critical", "disk", EV_ERR },
    { "Corruption", "disk", EV_ERR },
    { "filesystem read-only", "disk", EV_ERR },
    { "forced readonly", "disk", EV_ERR },
    { "has been shut down", "disk", EV_ERR },
    { "exception Emask", "disk", EV_WARN },
    { "hard resetting link", "disk", EV_WARN },
    { "controller is down", "disk", EV_ERR },
    { "resetting controller", "disk", EV_WARN },
    { "I/O timeout", "disk", EV_WARN },
    { "GPU HANG", "gpu", EV_ERR },
    { "GPU hang", "gpu", EV_ERR },
    { "GPU reset", "gpu", EV_ERR },
    { "ring gfx timeout", "gpu", EV_ERR },
    { "NVRM: Xid", "gpu", EV_ERR },
    { "AER:", "pcie", EV_WARN },
    { "PCIe Bus Error", "pcie", EV_WARN },
    { "segfault at", "crash", EV_WARN },
    { "general protection", "crash", EV_WARN },
    { "trap invalid opcode", "crash", EV_WARN },
    { "soft lockup", "crash", EV_ERR },
    { "hard LOCKUP", "crash", EV_ERR },
    { "blocked for more than", "crash", EV_WARN },
    { "self-detected stall", "crash", EV_ERR },
    { "Oops", "crash", EV_ERR },
    { "BUG:", "crash", EV_ERR },
    { "Kernel panic", "crash", EV_ERR },
    /* a USB device that doesn't connect properly: a cable or a port, almost
     * never the computer; the kernel logs these at "error" level */
    { "device descriptor read", "usb", EV_WARN },
    { "device not accepting address", "usb", EV_WARN },
    { "unable to enumerate USB device", "usb", EV_WARN },
    { "can't set config", "usb", EV_WARN },
    { "Maybe the USB cable is bad", "usb", EV_WARN },
    { "over-current", "usb", EV_WARN },
    { "under-voltage", "power", EV_ERR },
    { "Under-voltage", "power", EV_ERR },
    { "PM: suspend entry", "power", EV_INFO },
    { "PM: suspend exit", "power", EV_INFO },
};

static const char *guess_cat(const char *m)
{
    static const struct {
        const char *needle, *cat;
    } G[] = { { "nvme", "disk" },  { "ata", "disk" },    { "sd ", "disk" },    { "EXT4", "disk" },
              { "XFS", "disk" },   { "i915", "gpu" },    { "amdgpu", "gpu" },  { "nouveau", "gpu" },
              { "drm", "gpu" },    { "NVRM", "gpu" },    { "pcieport", "pcie" }, { "mce", "memory" },
              { "usb", "usb" },    { "thermal", "thermal" }, { "ACPI", "acpi" } };
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++)
        if (strstr(m, G[i].needle))
            return G[i].cat;
    return "kernel";
}

/* "prio,seq,usec,flags[,...];text" */
static void kmsg_record(health_t *h, char *rec, double boot_wall)
{
    char *semi = strchr(rec, ';');
    unsigned prio;
    unsigned long long seq, us;
    if (!semi || sscanf(rec, "%u,%llu,%llu", &prio, &seq, &us) != 3)
        return;
    if (prio >> 3) /* written by user space (systemd, ...), not by the kernel */
        return;
    int klevel = (int)(prio & 7);
    char *msg = semi + 1;
    msg[strcspn(msg, "\n")] = '\0';
    const char *cat = NULL;
    int level = -1;
    for (size_t i = 0; i < sizeof KMSG / sizeof KMSG[0]; i++)
        if (strstr(msg, KMSG[i].needle)) {
            cat = KMSG[i].cat;
            level = KMSG[i].level;
            break;
        }
    if (!cat) {
        if (klevel > 3) /* only errors and worse without a matching keyword */
            return;
        cat = guess_cat(msg);
        /* firmware tables with bugs: noisy at every boot, almost always harmless;
         * USB devices that misbehave: worth knowing, not a fault of the machine */
        level = !strncmp(msg, "ACPI", 4) || strstr(msg, "firmware") ? EV_INFO : !strcmp(cat, "usb") ? EV_WARN : EV_ERR;
    }
    if (level == EV_ERR)
        h->kmsg_err++;
    else if (level == EV_WARN)
        h->kmsg_warn++;
    double t = boot_wall + (double)us / 1e6;
    if (level == EV_ERR && t > h->kmsg_last)
        h->kmsg_last = t;
    char text[200];
    sfmt(text, sizeof text, "%s", msg);
    ev_add(h, t, level, cat, text);
}

/* ------------------------------------------------- events in plain words */

/* "usb 3-7: ..." -> is port 3-7 a socket on the outside or a built-in device? */
static const char *usb_port_kind(const char *text, char *port, size_t pn)
{
    int bus, p, len = 0;
    port[0] = '\0';
    if (sscanf(text, "usb %d-%d%n", &bus, &p, &len) != 2 || bus <= 0 || p <= 0)
        return NULL;
    sfmt(port, pn, "%d-%d", bus, p);
    if (text[len] != ':') /* "3-7.2": behind a hub */
        return "behind a hub";
    char path[PATHBUF], b[32];
    sfmt(path, sizeof path, "%s/bus/usb/devices/usb%d/%d-0:1.0/usb%d-port%d/connect_type", g_sysfs, bus, bus, bus, p);
    if (read_line(path, b, sizeof b))
        return NULL;
    return !strcmp(b, "hotplug") ? "an outside socket" : !strcmp(b, "hardwired") ? "built in" : NULL;
}

int event_explain(const char *cat, const char *text, char *out, size_t n)
{
    out[0] = '\0';
    if (!strcmp(cat, "usb") && strstr(text, " at USB 2 speed, though it can do ")) {
        str_copy(out, n, "It can do USB 3 but connected at USB 2 speed, so copies to and from it are many times slower "
                         "(USB 2 manages about 40 MB/s). Usually a USB 2 cable, hub or extension on the way, or a plug "
                         "not pushed in all the way. Plug it in again firmly, or try another cable or port.");
        return 1;
    }
    if (!strcmp(cat, "usb") && strstr(text, " dropped off and came back")) {
        str_copy(out, n, "The device lost its connection for a moment and connected again. If nobody touched it: a loose "
                         "plug or cable, a worn socket, or too little power (a powered hub helps drives). A drive that "
                         "drops off while being written to can lose what was being written.");
        return 1;
    }
    if (!strcmp(cat, "usb") && strncmp(text, "usb ", 4)) /* plugged in or out: nothing to explain */
        return 0;
    if (!strcmp(cat, "usb")) {
        char port[24], where[160] = "";
        const char *kind = usb_port_kind(text, port, sizeof port);
        if (kind && !strcmp(kind, "built in"))
            sfmt(where, sizeof where, " Port %s is built in (a camera, Bluetooth or card reader): if it repeats, that part "
                                      "has trouble.", port);
        else if (kind)
            sfmt(where, sizeof where, " Port %s is %s.", port, kind);
        if (strstr(text, "over-current"))
            sfmt(out, n, "A USB device drew more current than its port allows, so the port was switched off: unplug it or "
                         "use a powered hub.%s", where);
        else
            sfmt(out, n, "A USB device%s%s didn't answer properly while connecting: usually a loose or poor cable, a "
                         "worn port, or too little power for it. Once, while plugging something in, is harmless.%s",
                 port[0] ? " on port " : "", port, where);
        return 1;
    }
    const char *x = strstr(text, "Xid");
    if (x && strstr(text, "NVRM")) {
        const char *c = strstr(x, "): ");
        int code = c ? atoi(c + 3) : 0;
        if (code == 13 || code == 31 || code == 43)
            sfmt(out, n, "The NVIDIA driver stopped a program's GPU work (Xid %d): nearly always a bug in that program or "
                         "game, not the hardware.", code);
        else if (code == 79)
            str_copy(out, n, "The NVIDIA GPU stopped answering (Xid 79, \"fallen off the bus\"): usually power delivery, "
                             "heat, or a loose card or riser.");
        else if (code == 119 || code == 120)
            sfmt(out, n, "The GPU's own firmware stopped answering (Xid %d): a driver problem; a driver update often "
                         "helps.", code);
        else
            sfmt(out, n, "The NVIDIA driver reported a GPU error (Xid %d); NVIDIA's list of Xid codes says what it "
                         "means.", code);
        return 1;
    }
    if (strstr(text, "segfault at") || strstr(text, "general protection") || strstr(text, "trap invalid opcode")) {
        str_copy(out, n, "A program crashed: a bug in it, almost never the hardware - unless many different programs "
                         "crash, which points at memory.");
        return 1;
    }
    if (strstr(text, "blocked for more than")) {
        str_copy(out, n, "A task waited for minutes, nearly always for a disk, a USB drive or a network share that "
                         "stopped answering.");
        return 1;
    }
    if (!strcmp(cat, "disk") && strstr(text, "I/O error")) {
        str_copy(out, n, "A disk failed to read or write. Look at its health in the Hardware view (D runs its own "
                         "test); a loose cable does this too.");
        return 1;
    }
    if (strstr(text, "temperature above threshold") || strstr(text, "clock throttled")) {
        str_copy(out, n, "The CPU got hot enough to slow itself down: dust in the cooler, a blocked vent or dried "
                         "thermal paste are the usual causes.");
        return 1;
    }
    if (strstr(text, "Out of memory") || strstr(text, "oom-killer") || strstr(text, "Killed process")) {
        str_copy(out, n, "Memory ran out, and the kernel ended a program to free some.");
        return 1;
    }
    if (strstr(text, "AER:") || strstr(text, "PCIe Bus Error")) {
        if (strstr(text, "Corrected") || strstr(text, "corrected"))
            str_copy(out, n, "A PCIe device had a transfer error that was fixed on the spot: harmless unless it "
                             "happens all the time.");
        else
            str_copy(out, n, "A PCIe device had an error that could not be fixed; look at the device it names and "
                             "its slot or cable.");
        return 1;
    }
    if (strstr(text, "soft lockup") || strstr(text, "hard LOCKUP") || strstr(text, "self-detected stall")) {
        str_copy(out, n, "A CPU was stuck for many seconds, usually in a driver; a kernel or driver update may help.");
        return 1;
    }
    if (strstr(text, "Machine check") || strstr(text, "Hardware Error") || strstr(text, "EDAC")) {
        str_copy(out, n, "The CPU or the memory reported a hardware error; if it repeats, test the memory "
                         "(memtest86+).");
        return 1;
    }
    if (strstr(text, "nder-voltage")) {
        str_copy(out, n, "The power supply delivered too little voltage: use a stronger power supply.");
        return 1;
    }
    if (!strcmp(cat, "slow")) {
        str_copy(out, n, "hsm reads everything in turn, so its screen waits for a slow reading. On laptops the "
                         "firmware's sensors (fans, battery, ACPI) are the usual cause; if it keeps happening, hide "
                         "that sensor in Setup > Sensor names and limits.");
        return 1;
    }
    return 0;
}

static void kmsg_open(health_t *h)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/kmsg", g_devfs);
    h->kmsg_fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (h->kmsg_fd < 0) { /* dmesg_restrict=1 (Ubuntu, Debian, Fedora...): root only */
        h->kmsg = (errno == EPERM || errno == EACCES) ? -1 : 0;
        return;
    }
    struct stat st;
    h->kmsg_file = fstat(h->kmsg_fd, &st) == 0 && S_ISREG(st.st_mode); /* test hook */
    h->kmsg = 1;
}

static void kmsg_sample(health_t *h)
{
    if (h->kmsg != 1)
        return;
    /* message times are microseconds since boot (CLOCK_MONOTONIC) */
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    double boot_wall = wall_now() - ((double)mono.tv_sec + (double)mono.tv_nsec / 1e9);
    static char rec[8192];
    if (h->kmsg_file) { /* one record per line, appended to over time */
        ssize_t n = pread(h->kmsg_fd, rec, sizeof rec - 1, h->kmsg_off);
        if (n <= 0)
            return;
        rec[n] = '\0';
        char *cur = rec, *nl;
        while ((nl = strchr(cur, '\n')) != NULL) {
            *nl = '\0';
            h->kmsg_off += (long)(nl - cur) + 1;
            if (*cur != ' ')
                kmsg_record(h, cur, boot_wall);
            cur = nl + 1;
        }
        return;
    }
    for (int k = 0; k < 20000; k++) { /* the whole backlog on the first call */
        ssize_t n = read(h->kmsg_fd, rec, sizeof rec - 1);
        if (n < 0) {
            if (errno == EPIPE) /* messages were overwritten before we read them */
                continue;
            if (errno == EINTR)
                continue;
            break; /* EAGAIN: nothing more */
        }
        if (n == 0)
            break;
        rec[n] = '\0';
        kmsg_record(h, rec, boot_wall);
    }
}

/* ------------------------------------------------- freezes and suspends */

static int suspend_fails(unsigned long long *out)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/power/suspend_stats/fail", g_sysfs);
    return read_ull(p, out);
}

static void clock_hm(double wall, char *out, size_t n)
{
    time_t t = (time_t)wall;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%H:%M", &tm);
}

/* what the system was busy with while it stood still, from the pressure
 * stall figures (the kernel's 10 s average still holds the freeze) */
static void freeze_cause(const health_t *h, const procs_t *ps, char *out, size_t n)
{
    const psi_t *c = &h->psi[0], *m = &h->psi[1], *io = &h->psi[2];
    char who[80];
    out[0] = '\0';
    if (m->ok && (MAX(m->full, m->favg10) >= 10 || MAX(m->some, m->avg10) >= 40)) {
        health_blame(ps, BLAME_MEM, who, sizeof who);
        sfmt(out, n, ": memory was full%s", who);
    } else if (io->ok && MAX(io->full, io->favg10) >= 10) {
        health_blame(ps, BLAME_IO, who, sizeof who);
        sfmt(out, n, ": everything was waiting for a disk%s", who);
    } else if (c->ok && MAX(c->some, c->avg10) >= 60) {
        health_blame(ps, BLAME_CPU, who, sizeof who);
        sfmt(out, n, ": the CPU was overloaded%s", who);
    }
}

/* ---------------------------------------------- sleep and the battery */

#define WAKE_SETTLE 5.0 /* s after waking before the battery's charge is read again */

static void bat_snap(const power_t *p, batsnap_t *o)
{
    double pct = 0, wh = 0, full = 0;
    int n = 0, nwh = 0, nfull = 0, busy = 0;
    memset(o, 0, sizeof *o);
    o->wh = -1;
    for (int i = 0; i < p->nbat; i++) {
        const battery_t *b = &p->bat[i];
        if (b->capacity < 0)
            continue;
        n++;
        pct += b->capacity;
        if (!strcmp(b->status, "Charging") || !strcmp(b->status, "Full") || !strcmp(b->status, "Not charging"))
            busy = 1;
        if (b->energy_wh >= 0) {
            wh += b->energy_wh;
            nwh++;
        }
        if (b->energy_full_wh > 0) {
            full += b->energy_full_wh;
            nfull++;
        }
    }
    if (!n)
        return;
    o->ok = 1;
    o->charger = p->ac_online == 1 || (p->ac_online < 0 && busy);
    o->pct = pct / n;
    if (nwh == n) {
        o->wh = wh;
        if (n > 1 && nfull == n) /* two batteries: the share of what both can hold */
            o->pct = CLAMP(100.0 * wh / full, 0, 100);
    }
}

/* what the batteries lost while the computer slept, for the "woke up" event */
static void sleep_battery(const batsnap_t *a, const batsnap_t *b, double dur, char *out, size_t n)
{
    out[0] = '\0';
    if (!a->ok || !b->ok)
        return;
    long pa = lround(a->pct), pb = lround(b->pct);
    if (a->charger || b->charger) {
        if (pb > pa)
            sfmt(out, n, "; the battery charged from %ld%% to %ld%%", pa, pb);
        return;
    }
    if (pb == pa) {
        if (dur >= 1800) /* a long sleep that cost nothing worth a percent */
            sfmt(out, n, "; the battery stayed at %ld%%", pb);
        return;
    }
    if (pb > pa) { /* the gauge settled after a rest; no rate to give */
        sfmt(out, n, "; the battery went from %ld%% to %ld%%", pa, pb);
        return;
    }
    char rate[64] = "";
    double hours = dur / 3600.0;
    if (dur >= 1200) { /* long enough for a rate to mean something */
        double w = a->wh >= 0 && b->wh >= 0 && a->wh >= b->wh ? (a->wh - b->wh) / hours : -1;
        if (w >= 0)
            sfmt(rate, sizeof rate, ", %.1f%% an hour (%.*f W)", (a->pct - b->pct) / hours, w < 1 ? 2 : 1, w);
        else
            sfmt(rate, sizeof rate, ", %.1f%% an hour", (a->pct - b->pct) / hours);
    }
    sfmt(out, n, "; the battery went from %ld%% to %ld%%%s", pa, pb, rate);
}

static void wake_tell(health_t *h, const batsnap_t *post)
{
    char d[24], a[16], b[16], bat[128];
    fmt_duration(h->wake.dur, d, sizeof d);
    clock_hm(h->wake.wall, a, sizeof a);
    clock_hm(h->wake.wall + h->wake.dur, b, sizeof b);
    sleep_battery(&h->wake.pre, post, h->wake.dur, bat, sizeof bat);
    health_event(h, EV_INFO, "power", "Woke up after %s asleep (suspended at %s, woke at %s)%s", d, a, b, bat);
    h->wake.on = 0;
}

static void freeze_events(health_t *h, const procs_t *ps, double now, double mono)
{
    fz_t z;
    while (freeze_take(&z)) {
        if (z.kind == FZ_SUSPEND) {
            h->suspends++;
            h->asleep_total += z.dur;
            h->resume_t = now;
            if (h->wake.on) /* slept again before the last wake-up was told */
                wake_tell(h, &h->bat_prev);
            /* told a few seconds later, once the battery has reported its charge again */
            h->wake.on = 1;
            h->wake.dur = z.dur;
            h->wake.wall = z.wall;
            h->wake.due = mono + WAKE_SETTLE;
            h->wake.pre = h->bat_prev;
            if (h->susp_ok) /* a suspend that went well: not a failure to explain a freeze with */
                suspend_fails(&h->susp_fail);
            continue;
        }
        /* a suspend that failed half way stops every program for a moment too */
        unsigned long long f;
        if (h->susp_ok && suspend_fails(&f) == 0 && f != h->susp_fail) {
            char step[32] = "", dev[48] = "", p[PATHBUF];
            h->susp_fail = f;
            sfmt(p, sizeof p, "%s/power/suspend_stats/last_failed_step", g_sysfs);
            read_line(p, step, sizeof step);
            sfmt(p, sizeof p, "%s/power/suspend_stats/last_failed_dev", g_sysfs);
            read_line(p, dev, sizeof dev);
            health_event(h, EV_WARN, "power", "Suspend failed%s%s%s%s", step[0] ? " at the " : "", step,
                         dev[0] ? " step, because of " : "", dev);
            continue;
        }
        h->freezes++;
        h->freeze_last = z.dur;
        h->freeze_t = now;
        h->frozen_total += z.dur;
        h->freeze_max = MAX(h->freeze_max, z.dur);
        char why[160];
        freeze_cause(h, ps, why, sizeof why);
        health_event(h, z.dur >= 5 ? EV_ERR : EV_WARN, "freeze", "System froze for %.1f s%s%s", z.dur,
                     h->resume_t > 0 && now - h->resume_t < 60 ? " just after waking up" : "", why);
    }
}

/* --------------------------------------------------------- your limits */

#define LIMIT_FOR 5.0 /* past the limit this long before it counts */

static void limit_events(health_t *h, sensors_t *S, double now, double mono)
{
    static const char *const unit[] = { "°C", "RPM", "W", "V" };
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if ((t->ulo <= 0 && t->uhi <= 0) || !t->ok || t->hidden)
            continue;
        int low = t->ulo > 0 && t->value < t->ulo && t->type != SENS_TEMP && t->type != SENS_POWER;
        int high = t->uhi > 0 && t->value >= t->uhi && t->type != SENS_FAN;
        if (!low && !high) {
            /* back inside, with a little room, before it can count again */
            double m = t->type == SENS_FAN ? t->ulo * 0.05 : t->type == SENS_VOLT ? 0.01 : 2;
            if ((t->ulo <= 0 || t->value >= t->ulo + m) && (t->uhi <= 0 || t->value <= t->uhi - m))
                t->lim_flagged = 0;
            t->lim_t = 0;
            continue;
        }
        if (!t->lim_t)
            t->lim_t = mono;
        if (mono - t->lim_t < LIMIT_FOR)
            continue;
        h->ulim_last = now;
        if (t->lim_flagged)
            continue;
        t->lim_flagged = 1;
        char chip[24], v[24], l[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        int dec = t->type == SENS_VOLT ? 3 : 0;
        sfmt(v, sizeof v, "%.*f %s", dec, t->value, unit[t->type]);
        sfmt(l, sizeof l, "%.*f %s", t->type == SENS_VOLT ? 2 : 0, low ? t->ulo : t->uhi, unit[t->type]);
        char nm[80];
        if (t->name[0])
            str_copy(nm, sizeof nm, t->name);
        else
            sfmt(nm, sizeof nm, "%s %s", chip, t->label);
        static const char *const cat[] = { "thermal", "fan", "power", "voltage" };
        if (t->type == SENS_FAN)
            health_event(h, EV_WARN, cat[t->type], "%s turns at %s, below your minimum of %s: slowing down, dusty?", nm,
                         v, l);
        else
            health_event(h, EV_WARN, cat[t->type], "%s at %s, %s your %s of %s", nm, v, low ? "below" : "above",
                         low ? "minimum" : "limit", l);
    }
}

/* ------------------------------------------------------- fans stopping */

#define FAN_HOT 70.0 /* fans may rest when it's cool; at this temperature they must turn */

static void fan_events(health_t *h, sensors_t *S, double now, double mono)
{
    double cpu = sensors_cpu_temp(S);
    for (int i = 0; i < S->n; i++) {
        sensor_t *f = &S->s[i];
        if (f->type != SENS_FAN || !f->ok || f->hidden)
            continue;
        if (f->value >= 50) { /* turning */
            f->fan_seen = 1;
            f->fan_zero_t = 0;
            f->fan_flagged = 0;
            continue;
        }
        if (!f->fan_seen) /* never seen turning: probably nothing is plugged in there */
            continue;
        /* a graphics card's fan cools the card; any other fan the CPU (a
         * board chip's own temperature inputs are often unconnected junk) */
        double t = cpu;
        int gpu = !strncmp(f->chip, "amdgpu", 6) || !strncmp(f->chip, "radeon", 6) || !strncmp(f->chip, "nouveau", 7);
        if (gpu) {
            t = -1;
            for (int j = 0; j < S->n; j++)
                if (S->s[j].chip_idx == f->chip_idx && S->s[j].type == SENS_TEMP && S->s[j].ok)
                    t = MAX(t, S->s[j].value);
        }
        if (t < FAN_HOT) {
            f->fan_zero_t = 0;
            continue;
        }
        if (!f->fan_zero_t)
            f->fan_zero_t = mono;
        if (mono - f->fan_zero_t < 10) /* a moment at 0 can be a glitch of the reading */
            continue;
        h->fan_last = now;
        if (f->fan_flagged)
            continue;
        f->fan_flagged = 1;
        h->fan_stops++;
        char chip[24];
        sensors_chip_name(f->chip, chip, sizeof chip);
        health_event(h, EV_ERR, "fan", "%s %s stopped although the %s is at %.0f °C: is it blocked or broken?", chip,
                     f->name[0] ? f->name : f->label, gpu ? "GPU" : "CPU", t);
    }
}

/* ------------------------------------------------------ network links */

static void link_events(health_t *h, net_t *nt, double now)
{
    /* every link goes down while the machine sleeps */
    int resumed = h->resume_t > 0 && now - h->resume_t < 60;
    for (int i = 0; i < nt->n; i++) {
        netif_t *f = &nt->i[i];
        if (f->is_virtual || f->link_downs < 0)
            continue;
        if (f->downs_seen < 0 || resumed || f->link_downs < f->downs_seen) {
            f->downs_seen = f->link_downs;
            f->down_t = f->up ? 0 : -1; /* -1: down, but not news */
            continue;
        }
        const char *nm = f->name;
        char what[48];
        sfmt(what, sizeof what, f->wireless ? "Wi-Fi %s" : "%s", nm);
        long long k = f->link_downs - f->downs_seen;
        f->downs_seen = f->link_downs;
        if (k > 0) {
            char p[PATHBUF];
            unsigned fl = 1;
            sfmt(p, sizeof p, "%s/class/net/%s/flags", g_sysfs, nm);
            if (read_hex(p, &fl) == 0 && !(fl & 1)) { /* switched off, not lost */
                f->down_t = -1;
                continue;
            }
            h->link_drops += (int)k;
            h->link_last = now;
            if (!f->up) {
                f->down_t = now;
                if (k > 1)
                    health_event(h, EV_WARN, "net", "%s lost its link (%lld times)", what, k);
                else if (f->wireless)
                    health_event(h, EV_WARN, "net", "%s disconnected", what);
                else
                    health_event(h, EV_WARN, "net", "%s lost its link: cable, switch or router?", what);
            } else {
                f->down_t = 0;
                health_event(h, EV_WARN, "net", "%s dropped its link %lld time%s for a moment", what, k,
                             k == 1 ? "" : "s");
            }
        } else if (f->up && f->down_t != 0) {
            if (f->down_t > 0) {
                char d[24];
                fmt_duration(now - f->down_t, d, sizeof d);
                health_event(h, EV_INFO, "net", "%s link back after %s", what, d);
            }
            f->down_t = 0;
        }
    }
}

/* ------------------------------------------------- read-only file systems */

/* ext4 and others switch to read-only after errors (errors=remount-ro):
 * from then on every write fails, often silently for the user */
static void fs_events(health_t *h, const disks_t *ds, double now)
{
    static struct {
        char mount[128];
        int ro, flagged, seen;
    } m[MAX_FS * 2];
    static int nm;
    for (int j = 0; j < nm; j++)
        m[j].seen = 0;
    for (int i = 0; i < ds->nfs; i++) {
        const fs_t *f = &ds->fs[i];
        int j = 0;
        while (j < nm && strcmp(m[j].mount, f->mount))
            j++;
        if (j == nm) { /* new: whatever it is now is how it was mounted */
            if (nm == MAX_FS * 2)
                continue;
            nm++;
            str_copy(m[j].mount, sizeof m[j].mount, f->mount);
            m[j].ro = f->readonly;
            m[j].flagged = 0;
        }
        m[j].seen = 1;
        if (f->readonly && !m[j].ro) {
            m[j].flagged = 1;
            int err = h->kmsg_last > 0 && now - h->kmsg_last < 300;
            health_event(h, err ? EV_ERR : EV_WARN, "disk", "%s (%s) became read-only%s", f->mount, f->dev,
                         err ? " after kernel errors: the disk or the file system may be failing"
                             : ": remounted by hand, or after errors");
        }
        if (!f->readonly)
            m[j].flagged = 0;
        else if (m[j].flagged)
            h->ro_last = now;
        m[j].ro = f->readonly;
    }
    for (int j = 0; j < nm; j++) /* unmounted */
        if (!m[j].seen)
            m[j--] = m[--nm];
}

/* ----------------------------------------------------------------- main */

void health_init(health_t *h)
{
    h->kmsg_fd = -1;
    h->susp_ok = suspend_fails(&h->susp_fail) == 0;
    kmsg_open(h);
    nvme_find(h);
    ata_find(h);
}

void health_close(health_t *h)
{
    if (h->kmsg_fd >= 0)
        close(h->kmsg_fd);
    h->kmsg_fd = -1;
}

static void temp_events(health_t *h, sensors_t *S, const procs_t *ps, double now)
{
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_TEMP || !t->ok || t->hidden || !sensor_plausible(t))
            continue;
        double lim = t->crit > 0 ? t->crit : (i == S->cpu_idx ? 100.0 : -1);
        if (lim <= 0 || t->value < lim - 2)
            continue;
        h->hot_last = now;
        if (now - t->hot_t < 300)
            continue;
        t->hot_t = now;
        char chip[24], who[80];
        sensors_chip_name(t->chip, chip, sizeof chip);
        /* who is heating it up: drives get hot from disk work, GPUs from GPU work */
        int what = strstr(t->chip, "nvme") || strstr(t->chip, "drivetemp") ? BLAME_IO
                 : strstr(t->chip, "amdgpu") || strstr(t->chip, "nouveau") || strstr(t->chip, "i915") ||
                   strstr(t->chip, "xe")
                     ? BLAME_GPU
                     : BLAME_CPU;
        health_blame(ps, what, who, sizeof who);
        if (t->name[0])
            health_event(h, EV_ERR, "thermal", "%s reached %.0f °C (limit %.0f °C)%s", t->name, t->value, lim, who);
        else
            health_event(h, EV_ERR, "thermal", "%s %s reached %.0f °C (limit %.0f °C)%s", chip, t->label, t->value,
                         lim, who);
    }
}

/* NVIDIA GPUs say why they run below full speed: an event when one has been
 * held back for a while (heat or its power limit is normal for a laptop
 * GPU under load: information; a hardware slowdown is a warning) */
static void gpu_events(health_t *h, gpus_t *gs, const procs_t *ps, double now, double mono)
{
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        if (!g->slow_now || !g->slow_since || mono - g->slow_since < 10 || now - g->slow_ev_t < 600)
            continue;
        g->slow_ev_t = now;
        char who[80], gn[16], why[64] = "";
        health_blame(ps, BLAME_GPU, who, sizeof who);
        sfmt(gn, sizeof gn, gs->n > 1 ? "GPU%d" : "The GPU", i);
        if ((g->slow_now & GS_HEAT) && g->temp_c >= 0)
            app_s(why, sizeof why, "heat (%.0f °C)", g->temp_c);
        else if (g->slow_now & GS_HEAT)
            app_s(why, sizeof why, "heat");
        if (g->slow_now & GS_POWER) {
            if (why[0])
                app_s(why, sizeof why, " and ");
            if (g->power_cap_w > 0)
                app_s(why, sizeof why, "its power limit (%.0f W)", g->power_cap_w);
            else
                app_s(why, sizeof why, "its power limit");
        }
        int hw = (g->reasons & 0xC0ULL) != 0; /* the hardware stepped in: thermal or power brake */
        health_event(h, hw ? EV_WARN : EV_INFO, "gpu", "%s is slowing itself down: %s%s", gn, why, who);
    }
}

/* ---------------------------------------------- the network check */

/* troubles found by the pings: the router (the local link, Wi-Fi or cable)
 * or only the internet (beyond your network); told when they start and end */
static void netq_events(health_t *h, const net_t *nt, double now)
{
    const netq_t *q = &nt->q;
    if (!q->on)
        return;
    int wifi = 0;
    char air[80] = ""; /* the Wi-Fi as it is: a weak signal or a crowded band explains most lag */
    for (int i = 0; i < nt->n; i++)
        if (!strcmp(nt->i[i].name, q->iface)) {
            const netif_t *f = &nt->i[i];
            wifi = f->wireless;
            if (wifi && f->wifi.ok && f->quality >= 0) {
                char sp[24] = "";
                if (f->wifi.tx_mbps > 0)
                    sfmt(sp, sizeof sp, ", %.0f Mb/s", f->wifi.tx_mbps);
                sfmt(air, sizeof air, "; Wi-Fi signal %.0f dBm, %s channel %d%s", f->signal_dbm, wifi_band(f->wifi.freq),
                     f->wifi.channel, sp);
            } else if (wifi && f->quality >= 0)
                sfmt(air, sizeof air, "; Wi-Fi signal %.0f dBm", f->signal_dbm);
        }
    char link[48];
    if (wifi)
        str_copy(link, sizeof link, "Wi-Fi");
    else if (q->iface[0])
        sfmt(link, sizeof link, "Network (%s)", q->iface);
    else
        str_copy(link, sizeof link, "Network");
    for (int k = 0; k < NQ_COUNT; k++) {
        const nqt_t *t = &q->t[k];
        if (t->trouble && t->episode != h->nq_seen[k]) { /* a new one */
            h->nq_seen[k] = t->episode;
            /* the router too: that's the local link's trouble, told by its own event */
            if (k == NQ_NET && q->t[NQ_ROUTER].trouble)
                continue;
            h->nq_open[k] = 1;
            char what[96];
            if (t->trouble == NQ_LOSS)
                sfmt(what, sizeof what, "%.0f%% of the pings to %s are lost", t->worst_loss,
                     k == NQ_ROUTER ? "the router" : t->addr);
            else if (t->normal_ms > 0)
                sfmt(what, sizeof what, "%s answers in %.0f ms, normally %.0f ms", k == NQ_ROUTER ? "the router" : t->addr,
                     t->worst_ms, t->normal_ms);
            else
                sfmt(what, sizeof what, "%s answers in %.0f ms", k == NQ_ROUTER ? "the router" : t->addr, t->worst_ms);
            if (k == NQ_ROUTER)
                health_event(h, EV_WARN, "net", "%s trouble: %s (%s)%s", link, what, t->addr, air);
            else
                health_event(h, EV_WARN, "net", "Internet trouble: %s, while the router answers normally: it's beyond your network",
                             what);
        } else if (h->nq_open[k] && !t->trouble) { /* over */
            h->nq_open[k] = 0;
            char d[24] = "";
            if (t->until > t->since && t->since > 0)
                fmt_duration(t->until - t->since, d, sizeof d);
            health_event(h, EV_INFO, "net", "%s back to normal%s%s", k == NQ_ROUTER ? link : "Internet",
                         d[0] ? " after " : "", d);
        }
    }
    (void)now;
}

/* ------------------------------------------------- CPU power limits */

/* The CPU held back by its power limit rather than by heat: the package
 * draws what the limit allows while its clocks stay below what they can do.
 * A sustained limit is an average, so the package sits close to it; above
 * it, up to the boost limit, the CPU is still boosting freely. */
static void power_limit_sample(health_t *h, const sys_t *s, double dt, double now, double mono)
{
    const power_t *p = &s->power;
    const cpulim_t *L = &p->lim;
    const cpu_t *c = &s->cpu;
    double lw = L->eff_long_w, sw = L->eff_short_w;
    h->pl_known = L->ok && p->pkg_w >= 0;
    if (!h->pl_known) {
        h->pl_now = 0;
        h->pl_since = 0;
        return;
    }
    int busy = c->total >= 10;
    int hot = h->thr_last > 0 && now - h->thr_last < 10;   /* that's heat, said elsewhere */
    int slow = c->hw_max <= 0 || c->mhz_max <= 0 || c->mhz_max < 0.95 * c->hw_max;
    int at = 0;
    if (busy && !hot && slow) {
        if (sw > 0 && p->pkg_w >= 0.93 * sw)
            at = PL_SHORT;
        else if (lw > 0 && p->pkg_w >= 0.93 * lw && p->pkg_w <= 1.07 * lw)
            at = PL_LONG;
    }
    if (at) {
        h->pl_last = mono;
        h->pl_kind = at;
        if (!h->pl_since)
            h->pl_since = mono;
    } else if (h->pl_since && mono - h->pl_last >= 3) { /* a stretch ends after 3 s off the limit */
        h->pl_since = 0;
        h->pl_told = 0;
    }
    h->pl_now = h->pl_since && mono - h->pl_since >= 3 ? h->pl_kind : 0;
    if (busy)
        h->pl_busy_s += dt;
    if (h->pl_now)
        h->pl_held_s += dt;
    /* an event once a stretch has lasted 10 s, at most every half hour: on a
     * laptop under load this is normal, but it explains a slow CPU */
    if (h->pl_now && !h->pl_told && mono - h->pl_since >= 10 && now - h->pl_ev_t >= 1800) {
        char who[80];
        h->pl_told = 1;
        h->pl_ev_t = now;
        health_blame(&s->procs, BLAME_CPU, who, sizeof who);
        if (h->pl_now == PL_SHORT)
            health_event(h, EV_INFO, "cpu", "The CPU is held back by its power limit for boosts (%.0f W), not by heat%s",
                         sw, who);
        else
            health_event(h, EV_INFO, "cpu", "The CPU is held back by its sustained power limit (%.0f W), not by heat%s",
                         lw, who);
    }
}

/* the slowest check of this sample, for "reading X took 3 s" */
static void slow_part(health_t *h, double t0, const char *what)
{
    double d = now_sec() - t0;
    if (d > h->slow_s) {
        h->slow_s = d;
        str_copy(h->slow, sizeof h->slow, what);
    }
}

void health_sample(sys_t *s)
{
    health_t *h = &s->health;
    double dt = s->dt, now = wall_now(), mono = now_sec(), t0;
    h->slow_s = 0;
    h->slow[0] = '\0';

    static const char *const psi_name[3] = { "cpu", "memory", "io" };
    for (int i = 0; i < 3; i++)
        psi_sample(&h->psi[i], psi_name[i], dt);

    throttle_sample(h, &s->cpu, &s->procs, dt, now);

    if (s->mem.oom_ok) {
        if (!h->primed)
            h->oom_start = h->oom_prev = s->mem.oom_kill;
        if (s->mem.oom_kill > h->oom_prev) {
            unsigned long long k = s->mem.oom_kill - h->oom_prev;
            char who[80];
            h->oom_last = now;
            health_blame(&s->procs, BLAME_MEM, who, sizeof who);
            health_event(h, EV_ERR, "memory", "Memory ran out: the kernel killed %llu process%s%s", k,
                         k == 1 ? "" : "es", who);
        }
        h->oom_prev = s->mem.oom_kill;
    }

    t0 = now_sec();
    kmsg_sample(h);
    slow_part(h, t0, "the kernel log");
    temp_events(h, &s->sens, &s->procs, now);
    freeze_events(h, &s->procs, now, mono);
    batsnap_t bat;
    bat_snap(&s->power, &bat);
    if (h->wake.on && mono >= h->wake.due)
        wake_tell(h, &bat);
    h->bat_prev = bat;
    fan_events(h, &s->sens, now, mono);
    limit_events(h, &s->sens, now, mono);
    link_events(h, &s->net, now);
    netq_events(h, &s->net, now);
    for (int i = 0; i < s->gpus.n; i++) { /* a screen on the GPU that could sleep: it can't any more */
        gpu_t *g = &s->gpus.g[i];
        char now_on[128] = "";
        for (int k = 0; k < g->nscr; k++)
            if (g->scr[k].on && strlen(now_on) + strlen(g->scr[k].conn) + 2 < sizeof now_on) {
                strcat(now_on, g->scr[k].conn);
                strcat(now_on, ",");
            }
        char held[64];
        if (g->scr_primed && gpu_screen_holds(g, held, sizeof held)[0])
            for (int k = 0; k < g->nscr; k++) {
                char key[32];
                sfmt(key, sizeof key, "%s,", g->scr[k].conn);
                if (g->scr[k].on && !strstr(g->scr_told, key)) {
                    health_event(h, EV_INFO, "gpu", "A screen on %s%s%s%s keeps the %s GPU awake: that port is wired to it",
                                 g->scr[k].conn, g->scr[k].monitor[0] ? " (" : "", g->scr[k].monitor,
                                 g->scr[k].monitor[0] ? ")" : "", g->vendor);
                    break;
                }
            }
        if (g->scr_t > 0) {
            str_copy(g->scr_told, sizeof g->scr_told, now_on);
            g->scr_primed = 1;
        }
    }
    for (int i = 0; i < s->net.n; i++) { /* Wi-Fi moved to another band */
        char moved[160];
        if (s->net.i[i].wireless && wifi_band_moved(&s->net.i[i], mono, moved, sizeof moved))
            health_event(h, EV_INFO, "net", "%s", moved);
    }
    fs_events(h, &s->disks, now);
    gpu_events(h, &s->gpus, &s->procs, now, mono);
    power_limit_sample(h, s, dt, now, mono);
    leak_sample(s);
    runaway_sample(s);

    if (mono >= h->next_slow) { /* every 5 s */
        h->next_slow = mono + 5.0;
        t0 = now_sec();
        aer_sample(h, now);
        slow_part(h, t0, "the PCIe error counters");
        char p[PATHBUF], b[128];
        sfmt(p, sizeof p, "%s/sys/fs/file-nr", g_procfs);
        unsigned long long a = 0, u = 0, m = 0;
        if (read_file(p, b, sizeof b) > 0 && sscanf(b, "%llu %llu %llu", &a, &u, &m) == 3) {
            h->files = a;
            h->files_max = m;
        }
        if (h->governor[0] && s->cpu.governor[0] && strcmp(h->governor, s->cpu.governor))
            health_event(h, EV_INFO, "cpu", "CPU governor changed from %s to %s", h->governor, s->cpu.governor);
        str_copy(h->governor, sizeof h->governor, s->cpu.governor);
    }
    if (h->nnvme && mono >= h->next_nvme) {
        h->next_nvme = mono + NVME_EVERY;
        t0 = now_sec();
        nvme_sample(h, &s->procs);
        slow_part(h, t0, "the NVMe drives' health");
    }
    if (h->nsata && mono >= h->next_sata) {
        h->next_sata = mono + NVME_EVERY;
        t0 = now_sec();
        ata_sample(h);
        slow_part(h, t0, "the SATA drives' health");
    }
    drives_tick(h); /* self-tests hsm started */
    h->primed = 1;
}

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
/* log.c - record sensors and processes to CSV files.
 *
 * The system log has one column per selected "channel" (CPU usage, each
 * temperature / fan / power sensor, GPU, memory, disk and network rates...)
 * and one row per interval. Each logged process gets its own file. Rows are
 * flushed straight away and synced to disk every few seconds, so a freeze,
 * crash or power cut loses at most the last few seconds - which is the point
 * when you are hunting for overheating or overload problems. */
#define _GNU_SOURCE
#include "log.h"
#include "ui.h" /* config_as_user, config_mkdir_p */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

logger_t g_log = { .every = 1.0 };

enum {
    CH_CPU_TOTAL, CH_CPU_USER, CH_CPU_SYS, CH_CPU_IOWAIT, CH_CPU_CLOCK, CH_CPU_TEMP, CH_LOAD1,
    CH_CORE, CH_CORE_CLOCK, CH_MEM_PCT, CH_MEM_USED, CH_SWAP_USED,
    CH_GPU_LOAD, CH_GPU_TEMP, CH_GPU_POWER, CH_GPU_CLOCK, CH_GPU_VRAM,
    CH_POWER_TOTAL, CH_RAPL, CH_BAT_POWER, CH_BAT_CHARGE, CH_SENSOR,
    CH_DISK_R, CH_DISK_W, CH_DISK_BUSY, CH_NET_RX, CH_NET_TX,
    CH_THROTTLE, CH_PSI, CH_OOM, CH_PCIE, CH_NVME_USED, CH_NVME_MEDIA, CH_NVME_UNSAFE, CH_NVME_WARN, CH_NVME_HEAT,
    CH_SATA_WEAR, CH_SATA_REALLOC, CH_SATA_PENDING, CH_SATA_CRC,
    CH_DISK_RLAT, CH_DISK_WLAT, CH_NET_ERR, CH_NET_SIGNAL, CH_NET_LINK, CH_NET_FREQ, CH_WAKEUPS, CH_CSTATE, CH_FREEZE,
    CH_GPU_SLOW_HEAT, CH_GPU_SLOW_POWER, CH_CPU_PL, CH_GPU_ENC, CH_GPU_DEC, CH_GPU_PCIE_GEN, CH_GPU_PCIE_W,
    CH_PING, CH_PING_LOSS,
    CH_EVENTS /* stays last */
};

#define MB (1024.0 * 1024.0)
#define SYNC_EVERY 5.0 /* seconds between fsync() calls */

/* ------------------------------------------------------------ channels */

static void sanitize_id(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        *s = isalnum(c) || c == '.' || c == '-' ? (char)tolower(c) : '_';
    }
}

static chan_t *add(const char *group, int kind, int a, const char *name, const char *unit, const char *label,
                   const char *idfmt, ...) __attribute__((format(printf, 7, 8)));
static chan_t *add(const char *group, int kind, int a, const char *name, const char *unit, const char *label,
                   const char *idfmt, ...)
{
    if (g_log.nchan >= MAX_CHAN)
        return NULL;
    chan_t *c = &g_log.ch[g_log.nchan++];
    memset(c, 0, sizeof *c);
    va_list ap;
    va_start(ap, idfmt);
    vsnprintf(c->id, sizeof c->id, idfmt, ap);
    va_end(ap);
    sanitize_id(c->id);
    str_copy(c->group, sizeof c->group, group);
    str_copy(c->label, sizeof c->label, label);
    str_copy(c->unit, sizeof c->unit, unit);
    str_copy(c->name, sizeof c->name, name);
    for (char *q = c->label; *q; q++)
        if (*q == '"')
            *q = '\'';
    c->kind = kind;
    c->a = a;
    c->on = 1;
    return c;
}

static int in_list(const char *list, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        size_t l = strcspn(p, ",");
        size_t t = l;
        while (t && p[t - 1] == ' ')
            t--;
        if (t == n && !strncmp(p, id, n))
            return 1;
        p += l;
    }
    return 0;
}

void log_build_channels(sys_t *s, const char *skip)
{
    char lab[48];
    g_log.nchan = 0;

    add("CPU", CH_CPU_TOTAL, 0, "", "%", "CPU total", "cpu.total");
    add("CPU", CH_CPU_USER, 0, "", "%", "CPU user", "cpu.user");
    add("CPU", CH_CPU_SYS, 0, "", "%", "CPU system", "cpu.system");
    add("CPU", CH_CPU_IOWAIT, 0, "", "%", "CPU iowait", "cpu.iowait");
    if (s->cpu.mhz_avg > 0)
        add("CPU", CH_CPU_CLOCK, 0, "", "MHz", "Average CPU clock", "cpu.clock");
    if (s->sens.cpu_idx >= 0)
        add("CPU", CH_CPU_TEMP, 0, "", "C", "CPU temperature", "cpu.temp");
    add("CPU", CH_LOAD1, 0, "", "", "Load average (1 min)", "cpu.load1");
    for (int i = 0; i < s->cpu.ncpu; i++) {
        sfmt(lab, sizeof lab, "Core %d usage", s->cpu.id[i]);
        add("CPU cores", CH_CORE, s->cpu.id[i], "", "%", lab, "cpu.core%d", s->cpu.id[i]);
    }
    for (int i = 0; i < s->cpu.ncpu; i++)
        if (s->cpu.mhz[i] > 0) {
            sfmt(lab, sizeof lab, "Core %d clock", s->cpu.id[i]);
            add("CPU cores", CH_CORE_CLOCK, s->cpu.id[i], "", "MHz", lab, "cpu.core%d.clock", s->cpu.id[i]);
        }

    health_t *H = &s->health;
    if (H->thr_ok)
        add("Health", CH_THROTTLE, 0, "", "%", "CPU throttled (time)", "health.throttled");
    /* known only with the package's power (root): 100 = held back this sample */
    if (s->power.lim.ok && s->power.nrapl && !s->power.rapl_denied)
        add("Health", CH_CPU_PL, 0, "", "%", "CPU held back by power limit", "health.power_limited");
    static const char *const pn[3] = { "CPU", "Memory", "Disk I/O" }, *const pk[3] = { "cpu", "memory", "io" };
    for (int i = 0; i < 3; i++)
        if (H->psi[i].ok) {
            sfmt(lab, sizeof lab, "%s pressure", pn[i]);
            add("Health", CH_PSI, i * 2, "", "%", lab, "health.psi.%s", pk[i]);
            if (i && H->psi[i].has_full) {
                sfmt(lab, sizeof lab, "%s pressure (full)", pn[i]);
                add("Health", CH_PSI, i * 2 + 1, "", "%", lab, "health.psi.%s_full", pk[i]);
            }
        }
    if (s->mem.oom_ok)
        add("Health", CH_OOM, 0, "", "count", "OOM kills since boot", "health.oom");
    if (H->aer_ok)
        add("Health", CH_PCIE, 0, "", "count", "PCIe errors since boot", "health.pcie");
    add("Health", CH_FREEZE, 0, "", "s", "System frozen (total)", "health.frozen");
    for (int i = 0; i < H->nnvme; i++) {
        const char *n = H->nvme[i].name;
        if (H->nvme[i].state < 0) /* can't be read (needs root): no empty columns */
            continue;
        sfmt(lab, sizeof lab, "%s wear used", n);
        add("Health", CH_NVME_USED, i, "", "%", lab, "nvme.%s.used", n);
        sfmt(lab, sizeof lab, "%s media errors", n);
        add("Health", CH_NVME_MEDIA, i, "", "count", lab, "nvme.%s.media_errors", n);
        sfmt(lab, sizeof lab, "%s unsafe shutdowns", n);
        add("Health", CH_NVME_UNSAFE, i, "", "count", lab, "nvme.%s.unsafe", n);
        sfmt(lab, sizeof lab, "%s critical warning", n);
        add("Health", CH_NVME_WARN, i, "", "count", lab, "nvme.%s.warning", n);
        sfmt(lab, sizeof lab, "%s slowed by heat (total)", n);
        add("Health", CH_NVME_HEAT, i, "", "s", lab, "nvme.%s.heat_slowed", n);
    }
    for (int i = 0; i < H->nsata; i++) {
        const sata_t *d = &H->sata[i];
        if (d->state == -1 || d->state == -2)
            continue;
        if (!d->rotational) {
            sfmt(lab, sizeof lab, "%s wear used", d->name);
            add("Health", CH_SATA_WEAR, i, "", "%", lab, "sata.%s.wear", d->name);
        }
        sfmt(lab, sizeof lab, "%s reallocated sectors", d->name);
        add("Health", CH_SATA_REALLOC, i, "", "count", lab, "sata.%s.reallocated", d->name);
        sfmt(lab, sizeof lab, "%s pending sectors", d->name);
        add("Health", CH_SATA_PENDING, i, "", "count", lab, "sata.%s.pending", d->name);
        sfmt(lab, sizeof lab, "%s cable errors", d->name);
        add("Health", CH_SATA_CRC, i, "", "count", lab, "sata.%s.cable_errors", d->name);
    }
    if (s->cpu.nidle) {
        add("CPU idle", CH_WAKEUPS, 0, "", "/s", "Wake-ups", "cpu.wakeups");
        for (int k = 0; k < s->cpu.nidle; k++)
            if (strcmp(s->cpu.idle[k].name, "POLL")) {
                sfmt(lab, sizeof lab, "Idle in %s", s->cpu.idle[k].name);
                add("CPU idle", CH_CSTATE, k, "", "%", lab, "cpu.idle.%s", s->cpu.idle[k].name);
            }
    }

    add("Memory", CH_MEM_PCT, 0, "", "%", "Memory used", "mem.used_pct");
    add("Memory", CH_MEM_USED, 0, "", "MB", "Memory used", "mem.used");
    if (s->mem.swap_total)
        add("Memory", CH_SWAP_USED, 0, "", "MB", "Swap used", "mem.swap");

    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        char gn[8];
        sfmt(gn, sizeof gn, s->gpus.n > 1 ? "GPU%d" : "GPU", i);
        /* an NVIDIA GPU may be asleep when the log starts: its columns are
         * there anyway, for when it wakes up */
        int nv = g->nv_state != NV_NONE;
        if (g->util >= 0 || g->neng || !strcmp(g->driver, "i915") || nv) {
            sfmt(lab, sizeof lab, "%s load", gn);
            add("GPU", CH_GPU_LOAD, i, "", "%", lab, "gpu%d.load", i);
        }
        if (g->temp_c >= 0 || nv) {
            sfmt(lab, sizeof lab, "%s temperature", gn);
            add("GPU", CH_GPU_TEMP, i, "", "C", lab, "gpu%d.temp", i);
        }
        if (g->power_w >= 0 || nv) {
            sfmt(lab, sizeof lab, "%s power", gn);
            add("GPU", CH_GPU_POWER, i, "", "W", lab, "gpu%d.power", i);
        }
        if (g->clk_mhz >= 0 || nv) {
            sfmt(lab, sizeof lab, "%s clock", gn);
            add("GPU", CH_GPU_CLOCK, i, "", "MHz", lab, "gpu%d.clock", i);
        }
        if (g->mem_total > 0 || nv) {
            sfmt(lab, sizeof lab, "%s VRAM used", gn);
            add("GPU", CH_GPU_VRAM, i, "", "MB", lab, "gpu%d.vram", i);
        }
        if (nv) { /* held back, as the driver says: 100 = this whole sample, 0 = full speed */
            sfmt(lab, sizeof lab, "%s slowed by heat", gn);
            add("GPU", CH_GPU_SLOW_HEAT, i, "", "%", lab, "gpu%d.slow_heat", i);
            sfmt(lab, sizeof lab, "%s slowed by power limit", gn);
            add("GPU", CH_GPU_SLOW_POWER, i, "", "%", lab, "gpu%d.slow_power", i);
            sfmt(lab, sizeof lab, "%s video encode", gn);
            add("GPU", CH_GPU_ENC, i, "", "%", lab, "gpu%d.encode", i);
            sfmt(lab, sizeof lab, "%s video decode", gn);
            add("GPU", CH_GPU_DEC, i, "", "%", lab, "gpu%d.decode", i);
            sfmt(lab, sizeof lab, "%s PCIe link generation", gn);
            add("GPU", CH_GPU_PCIE_GEN, i, "", "", lab, "gpu%d.pcie_gen", i);
            sfmt(lab, sizeof lab, "%s PCIe link width", gn);
            add("GPU", CH_GPU_PCIE_W, i, "", "lanes", lab, "gpu%d.pcie_width", i);
        }
    }

    if (s->total_w >= 0)
        add("Power", CH_POWER_TOTAL, 0, "", "W", "Estimated total power", "power.total");
    for (int i = 0; i < s->power.nrapl; i++)
        if (s->power.z[i].readable) {
            sfmt(lab, sizeof lab, "RAPL %s", s->power.z[i].name);
            add("Power", CH_RAPL, i, "", "W", lab, "power.rapl.%s.%d", s->power.z[i].name, i);
        }
    for (int i = 0; i < s->power.nbat; i++) {
        sfmt(lab, sizeof lab, "%s drain", s->power.bat[i].name);
        add("Power", CH_BAT_POWER, i, "", "W", lab, "power.%s.drain", s->power.bat[i].name);
        sfmt(lab, sizeof lab, "%s charge", s->power.bat[i].name);
        add("Power", CH_BAT_CHARGE, i, "", "%", lab, "power.%s.charge", s->power.bat[i].name);
    }

    static const char *const unit[] = { "C", "RPM", "W", "V" };
    static const char *const grp[] = { "Temperatures", "Fans", "Power", "Voltages" };
    for (int i = 0; i < s->sens.n; i++) {
        sensor_t *t = &s->sens.s[i];
        if (t->hidden)
            continue;
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        if (t->name[0])
            str_copy(lab, sizeof lab, t->name);
        else
            sfmt(lab, sizeof lab, "%s %s", chip, t->label); /* "CPU Core 0", "NVMe Composite" */
        add(grp[t->type], CH_SENSOR, i, "", unit[t->type], lab, "sensor.%s.%s", t->chip, t->label);
    }

    for (int i = 0; i < s->disks.n; i++) {
        disk_t *d = &s->disks.d[i];
        sfmt(lab, sizeof lab, "%s read", d->name);
        add("Disks", CH_DISK_R, 0, d->name, "MB/s", lab, "disk.%s.read", d->name);
        sfmt(lab, sizeof lab, "%s write", d->name);
        add("Disks", CH_DISK_W, 0, d->name, "MB/s", lab, "disk.%s.write", d->name);
        sfmt(lab, sizeof lab, "%s busy", d->name);
        add("Disks", CH_DISK_BUSY, 0, d->name, "%", lab, "disk.%s.busy", d->name);
        sfmt(lab, sizeof lab, "%s read time", d->name);
        add("Disks", CH_DISK_RLAT, 0, d->name, "ms", lab, "disk.%s.read_ms", d->name);
        sfmt(lab, sizeof lab, "%s write time", d->name);
        add("Disks", CH_DISK_WLAT, 0, d->name, "ms", lab, "disk.%s.write_ms", d->name);
    }

    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        if (f->is_virtual && !f->up)
            continue;
        if (!strncmp(f->name, "veth", 4))
            continue;
        sfmt(lab, sizeof lab, "%s download", f->name);
        add("Network", CH_NET_RX, 0, f->name, "MB/s", lab, "net.%s.rx", f->name);
        sfmt(lab, sizeof lab, "%s upload", f->name);
        add("Network", CH_NET_TX, 0, f->name, "MB/s", lab, "net.%s.tx", f->name);
        sfmt(lab, sizeof lab, "%s errors+drops", f->name);
        add("Network", CH_NET_ERR, 0, f->name, "/s", lab, "net.%s.errors", f->name);
        if (f->wireless) {
            sfmt(lab, sizeof lab, "%s Wi-Fi signal", f->name);
            add("Network", CH_NET_SIGNAL, 0, f->name, "dBm", lab, "net.%s.signal", f->name);
            sfmt(lab, sizeof lab, "%s Wi-Fi link speed", f->name);
            add("Network", CH_NET_LINK, 0, f->name, "Mb/s", lab, "net.%s.link", f->name);
            sfmt(lab, sizeof lab, "%s Wi-Fi frequency", f->name);
            add("Network", CH_NET_FREQ, 0, f->name, "MHz", lab, "net.%s.freq", f->name);
        }
    }
    if (s->net.q.on) { /* the network check, when it runs as the log starts */
        add("Network", CH_PING, NQ_ROUTER, "", "ms", "Router ping", "net.ping.router");
        add("Network", CH_PING_LOSS, NQ_ROUTER, "", "%", "Router pings lost", "net.ping.router_loss");
        add("Network", CH_PING, NQ_NET, "", "ms", "Internet ping", "net.ping.internet");
        add("Network", CH_PING_LOSS, NQ_NET, "", "%", "Internet pings lost", "net.ping.internet_loss");
    }

    /* kernel errors, throttling, OOM kills... as text, in the last column */
    add("Events", CH_EVENTS, 0, "", "count", "Events", "events");

    for (int i = 0; i < g_log.nchan; i++)
        g_log.ch[i].on = !(skip && in_list(skip, g_log.ch[i].id));
}

void log_skip_string(char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0, k = 0; i < g_log.nchan; i++)
        if (!g_log.ch[i].on) {
            size_t l = strlen(out);
            if (l + strlen(g_log.ch[i].id) + 3 >= n)
                break;
            sfmt(out + l, n - l, "%s%s", k++ ? ", " : "", g_log.ch[i].id);
        }
}

int log_count_on(void)
{
    int k = 0;
    for (int i = 0; i < g_log.nchan; i++)
        k += g_log.ch[i].on;
    return k;
}

static int cpu_index(const sys_t *s, int id)
{
    for (int i = 0; i < s->cpu.ncpu; i++)
        if (s->cpu.id[i] == id)
            return i;
    return -1;
}

static double nn(double v) /* negative = unknown */
{
    return v < 0 ? NAN : v;
}

double log_chan_value(const sys_t *s, const chan_t *c)
{
    int i;
    switch (c->kind) {
    case CH_CPU_TOTAL: return s->cpu.total;
    case CH_CPU_USER: return s->cpu.user;
    case CH_CPU_SYS: return s->cpu.sys;
    case CH_CPU_IOWAIT: return s->cpu.iowait;
    case CH_CPU_CLOCK: return s->cpu.mhz_avg > 0 ? s->cpu.mhz_avg : NAN;
    case CH_CPU_TEMP: return nn(s->cpu.temp_c);
    case CH_LOAD1: return s->cpu.load[0];
    case CH_CORE: return (i = cpu_index(s, c->a)) >= 0 ? s->cpu.pct[i] : NAN;
    case CH_CORE_CLOCK: return (i = cpu_index(s, c->a)) >= 0 && s->cpu.mhz[i] > 0 ? s->cpu.mhz[i] : NAN;
    case CH_MEM_PCT: return s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : NAN;
    case CH_MEM_USED: return (double)s->mem.used / MB;
    case CH_SWAP_USED: return (double)s->mem.swap_used / MB;
    case CH_GPU_LOAD: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].util) : NAN;
    case CH_GPU_TEMP: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].temp_c) : NAN;
    case CH_GPU_POWER: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].power_w) : NAN;
    case CH_GPU_CLOCK: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].clk_mhz) : NAN;
    case CH_GPU_VRAM: return c->a < s->gpus.n && s->gpus.g[c->a].mem_used >= 0 ? s->gpus.g[c->a].mem_used / MB : NAN;
    case CH_POWER_TOTAL: return nn(s->total_w);
    case CH_RAPL: return c->a < s->power.nrapl ? nn(s->power.z[c->a].watts) : NAN;
    case CH_BAT_POWER: return c->a < s->power.nbat ? nn(s->power.bat[c->a].watts) : NAN;
    case CH_BAT_CHARGE: return c->a < s->power.nbat ? nn(s->power.bat[c->a].capacity) : NAN;
    case CH_SENSOR: return c->a < s->sens.n && s->sens.s[c->a].ok ? s->sens.s[c->a].value : NAN;
    case CH_DISK_R:
    case CH_DISK_W:
    case CH_DISK_BUSY:
        for (i = 0; i < s->disks.n; i++)
            if (!strcmp(s->disks.d[i].name, c->name))
                return c->kind == CH_DISK_R ? s->disks.d[i].rd_bps / MB
                     : c->kind == CH_DISK_W ? s->disks.d[i].wr_bps / MB : s->disks.d[i].util;
        return NAN;
    case CH_NET_RX:
    case CH_NET_TX:
    case CH_NET_ERR:
    case CH_NET_SIGNAL:
    case CH_NET_LINK:
    case CH_NET_FREQ:
        for (i = 0; i < s->net.n; i++)
            if (!strcmp(s->net.i[i].name, c->name)) {
                const netif_t *f = &s->net.i[i];
                switch (c->kind) {
                case CH_NET_RX: return f->rx_bps / MB;
                case CH_NET_TX: return f->tx_bps / MB;
                case CH_NET_ERR: return f->err_rate + f->drop_rate;
                case CH_NET_LINK: return f->wifi.ok && f->wifi.tx_mbps > 0 ? f->wifi.tx_mbps : NAN;
                case CH_NET_FREQ: return f->wifi.ok ? f->wifi.freq : NAN;
                default: return f->quality >= 0 ? f->signal_dbm : NAN;
                }
            }
        return NAN;
    case CH_DISK_RLAT:
    case CH_DISK_WLAT:
        for (i = 0; i < s->disks.n; i++)
            if (!strcmp(s->disks.d[i].name, c->name))
                return c->kind == CH_DISK_RLAT ? s->disks.d[i].rd_lat : s->disks.d[i].wr_lat;
        return NAN;
    case CH_THROTTLE: return s->health.thr_ok ? s->health.thr_pct : NAN;
    case CH_PSI: {
        const psi_t *p = &s->health.psi[c->a / 2];
        return p->ok ? (c->a & 1 ? p->full : p->some) : NAN;
    }
    case CH_OOM: return s->mem.oom_ok ? (double)s->mem.oom_kill : NAN;
    case CH_PCIE:
        return s->health.aer_ok ? (double)(s->health.pci_cor + s->health.pci_nonfatal + s->health.pci_fatal) : NAN;
    case CH_NVME_USED:
    case CH_NVME_MEDIA:
    case CH_NVME_UNSAFE:
    case CH_NVME_WARN:
    case CH_NVME_HEAT: {
        if (c->a >= s->health.nnvme || s->health.nvme[c->a].state != 1)
            return NAN;
        const nvme_t *v = &s->health.nvme[c->a];
        return c->kind == CH_NVME_USED     ? v->used
             : c->kind == CH_NVME_MEDIA    ? (double)v->media_err
             : c->kind == CH_NVME_UNSAFE   ? (double)v->unsafe
             : c->kind == CH_NVME_HEAT     ? (double)(v->heat_s[0] + v->heat_s[1])
                                           : (double)v->crit;
    }
    case CH_SATA_WEAR:
    case CH_SATA_REALLOC:
    case CH_SATA_PENDING:
    case CH_SATA_CRC: {
        if (c->a >= s->health.nsata)
            return NAN;
        const sata_t *d = &s->health.sata[c->a];
        if (d->hours < 0 && d->realloc < 0) /* never read */
            return NAN;
        long long v = c->kind == CH_SATA_WEAR ? d->wear : c->kind == CH_SATA_REALLOC ? d->realloc
                    : c->kind == CH_SATA_PENDING ? d->pending : d->crc;
        return v >= 0 ? (double)v : NAN;
    }
    case CH_WAKEUPS: return s->cpu.nidle ? s->cpu.wakeups : NAN;
    case CH_CSTATE: return c->a < s->cpu.nidle ? s->cpu.idle[c->a].pct : NAN;
    case CH_FREEZE: return s->health.frozen_total;
    case CH_GPU_SLOW_HEAT:
    case CH_GPU_SLOW_POWER: {
        if (c->a >= s->gpus.n)
            return NAN;
        const gpu_t *g = &s->gpus.g[c->a];
        if (!g->nvml) /* asleep or let go of: certainly not held back */
            return g->nv_state == NV_ASLEEP || g->nv_state == NV_IDLE ? 0 : NAN;
        if (!g->reasons_ok)
            return NAN;
        return g->slow_now & (c->kind == CH_GPU_SLOW_HEAT ? GS_HEAT : GS_POWER) ? 100 : 0;
    }
    case CH_CPU_PL: return s->health.pl_known ? (s->health.pl_now ? 100 : 0) : NAN;
    case CH_PING: return s->net.q.on && c->a < NQ_COUNT ? nn(s->net.q.t[c->a].rtt_ms) : NAN;
    case CH_PING_LOSS: return s->net.q.on && c->a < NQ_COUNT ? nn(s->net.q.t[c->a].loss_pct) : NAN;
    case CH_GPU_ENC: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].enc_pct) : NAN;
    case CH_GPU_DEC: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].dec_pct) : NAN;
    case CH_GPU_PCIE_GEN: return c->a < s->gpus.n && s->gpus.g[c->a].pcie_gen > 0 ? s->gpus.g[c->a].pcie_gen : NAN;
    case CH_GPU_PCIE_W: return c->a < s->gpus.n && s->gpus.g[c->a].pcie_width > 0 ? s->gpus.g[c->a].pcie_width : NAN;
    case CH_EVENTS: return (double)s->health.nev;
    }
    return NAN;
}

static int decimals(const char *unit)
{
    if (!strcmp(unit, "MHz") || !strcmp(unit, "RPM") || !strcmp(unit, "count") || !strcmp(unit, "dBm"))
        return 0;
    if (!strcmp(unit, "V"))
        return 3;
    if (!strcmp(unit, "MB/s"))
        return 3;
    if (!strcmp(unit, "W") || !*unit)
        return 2;
    return 1;
}

void log_fmt_value(const chan_t *c, double v, char *out, size_t n)
{
    if (isnan(v)) {
        str_copy(out, n, "-");
        return;
    }
    if (!strcmp(c->unit, "C"))
        sfmt(out, n, "%.1f °C", v);
    else if (!strcmp(c->unit, "count"))
        sfmt(out, n, "%.0f", v);
    else if (!strcmp(c->unit, "%"))
        sfmt(out, n, "%.1f %%", v);
    else if (*c->unit)
        sfmt(out, n, "%.*f %s", decimals(c->unit), v, c->unit);
    else
        sfmt(out, n, "%.2f", v);
}

/* --------------------------------------------------------------- files */

static double wall_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stamp_at(double wall, char *out, size_t n, const char *fmt)
{
    time_t t = (time_t)wall;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, fmt, &tm);
}

static void stamp(char *out, size_t n, const char *fmt)
{
    stamp_at(wall_time(), out, n, fmt);
}

int log_day_key(double wall)
{
    time_t t = (time_t)wall;
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* Create a new file (never overwrite one) in the folder of its day,
 * <log folder>/2026-11-12/, as the real user, also when hsm runs under
 * sudo, so the logs belong to whoever ran it. */
FILE *log_create(double wall, const char *base, char *path, size_t pn, char *err, size_t en)
{
    int who = config_as_user();
    if (who < 0) {
        sfmt(err, en, "Could not switch to your user to write the log");
        return NULL;
    }
    FILE *f = NULL;
    char day[16], dir[PATHBUF];
    stamp_at(wall, day, sizeof day, "%Y-%m-%d");
    sfmt(dir, sizeof dir, "%s/%s", g_log.dir, day);
    if (config_mkdir_p(dir) != 0) {
        sfmt(err, en, "Could not create %s: %s", dir, strerror(errno));
    } else {
        for (int k = 1; k < 100 && !f; k++) {
            if (k == 1)
                sfmt(path, pn, "%s/%s.csv", dir, base);
            else
                sfmt(path, pn, "%s/%s-%d.csv", dir, base, k);
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd < 0) {
                if (errno == EEXIST)
                    continue;
                sfmt(err, en, "Could not create %s: %s", path, strerror(errno));
                break;
            }
            f = fdopen(fd, "w");
            if (!f)
                close(fd);
        }
    }
    config_as_root(who);
    return f;
}

static FILE *open_new(const char *base, char *path, size_t pn, char *err, size_t en)
{
    return log_create(wall_time(), base, path, pn, err, en);
}

long log_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void put_num(FILE *f, double v, int dec)
{
    if (isnan(v) || isinf(v))
        fputc(',', f);
    else
        fprintf(f, ",%.*f", dec, v);
}

void log_put_value(FILE *f, const chan_t *c, double v)
{
    put_num(f, v, decimals(c->unit));
}

/* local time and its offset from UTC: the offset tells the two 02:30 rows
 * apart on the night the clocks go back, and makes the times unambiguous */
void log_put_time(FILE *f, double wall)
{
    time_t t = (time_t)wall;
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    long off = tm.tm_gmtoff / 60;
    fprintf(f, "%s,%c%02ld:%02ld", ts, off < 0 ? '-' : '+', labs(off) / 60, labs(off) % 60);
}

void log_put_header(FILE *f, const char *first, int all)
{
    fputs(first, f);
    for (int i = 0; i < g_log.nchan; i++) {
        chan_t *c = &g_log.ch[i];
        if (!c->on && !all)
            continue;
        if (*c->unit && strcmp(c->unit, "count"))
            fprintf(f, ",\"%s (%s)\"", c->label, c->unit);
        else
            fprintf(f, ",\"%s\"", c->label);
    }
    fputc('\n', f);
}

/* ------------------------------------------------------------ system log */

void log_note(int err, const char *fmt, ...)
{
    if (g_log.nnote == 4) { /* full: the oldest goes */
        memmove(g_log.note[0], g_log.note[1], sizeof g_log.note[0] * 3);
        memmove(g_log.note_err, g_log.note_err + 1, sizeof g_log.note_err[0] * 3);
        g_log.nnote--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log.note[g_log.nnote], sizeof g_log.note[0], fmt, ap);
    va_end(ap);
    g_log.note_err[g_log.nnote++] = err;
}

int log_note_pop(char *out, size_t n, int *err)
{
    if (!g_log.nnote)
        return 0;
    str_copy(out, n, g_log.note[0]);
    *err = g_log.note_err[0];
    memmove(g_log.note[0], g_log.note[1], sizeof g_log.note[0] * 3);
    memmove(g_log.note_err, g_log.note_err + 1, sizeof g_log.note_err[0] * 3);
    g_log.nnote--;
    return 1;
}
#define note log_note

int log_chan_is_events(const chan_t *c)
{
    return c->kind == CH_EVENTS;
}

/* the events numbered after..upto, as one quoted CSV field:
 * "12:03:44 disk: nvme0n1: I/O error ... | 12:03:50 thermal: ..." */
void log_put_events(FILE *f, const health_t *h, unsigned long after, unsigned long upto)
{
    fputs(",\"", f);
    int n = 0;
    for (int i = h->nev - 1; i >= 0; i--) { /* oldest first */
        const event_t *e = health_event_at(h, i);
        if (e->seq <= after || e->seq > upto)
            continue;
        char ts[16];
        time_t t = (time_t)e->t;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);
        fprintf(f, "%s%s %s: ", n++ ? " | " : "", ts, e->cat);
        for (const char *q = e->text; *q; q++) {
            if (*q == '"')
                fputc('"', f); /* CSV: quotes are doubled */
            if ((unsigned char)*q >= 32 || *q == '\t')
                fputc(*q, f);
        }
    }
    fputc('"', f);
}

static void sys_row(sys_t *s, double now)
{
    log_put_time(g_log.f, wall_time());
    fprintf(g_log.f, ",%.1f", now - g_log.t0);
    for (int i = 0; i < g_log.nchan; i++)
        if (g_log.ch[i].on) {
            if (g_log.ch[i].kind == CH_EVENTS) {
                log_put_events(g_log.f, &s->health, g_log.ev_seq, s->health.ev_seq);
                g_log.ev_seq = s->health.ev_seq;
            } else {
                put_num(g_log.f, log_chan_value(s, &g_log.ch[i]), decimals(g_log.ch[i].unit));
            }
        }
    fputc('\n', g_log.f);
    fflush(g_log.f);
    g_log.rows++;
    g_log.last_row = now;
    if (ferror(g_log.f)) {
        note(1, "Logging stopped: could not write %s (disk full?)", g_log.path);
        log_stop();
    }
}

static const char SYS_FIRST[] = "time,utc_offset,elapsed_s";

static int sys_open(char *err, size_t n)
{
    char ts[32], base[64];
    stamp(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S");
    sfmt(base, sizeof base, "hsm-system-%s", ts);
    g_log.f = open_new(base, g_log.path, sizeof g_log.path, err, n);
    if (!g_log.f)
        return -1;
    g_log.day = log_day_key(wall_time());
    log_put_header(g_log.f, SYS_FIRST, 0);
    return 0;
}

int log_start(sys_t *s, char *err, size_t n)
{
    if (g_log.f)
        return 0;
    if (!log_count_on()) {
        sfmt(err, n, "Nothing selected to log: pick values in the Logging view (5)");
        return -1;
    }
    if (sys_open(err, n) != 0)
        return -1;
    g_log.rows = 0;
    g_log.t0 = g_log.last_sync = now_sec();
    g_log.ev_seq = s->health.ev_seq; /* only what happens from now on */
    sys_row(s, g_log.t0); /* first row right away */
    return g_log.f ? 0 : -1;
}

void log_stop(void)
{
    if (!g_log.f)
        return;
    fflush(g_log.f);
    fsync(fileno(g_log.f));
    fclose(g_log.f);
    g_log.f = NULL;
}

/* ---------------------------------------------------------- process logs */

static proc_t *find_proc(sys_t *s, pid_t pid, unsigned long long start)
{
    int lo = 0, hi = s->procs.n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        proc_t *p = &s->procs.p[mid];
        if (p->pid == pid)
            return p->start == start ? p : NULL;
        if (p->pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

int log_proc_find(pid_t pid, unsigned long long start)
{
    for (int i = 0; i < g_log.nplog; i++)
        if (g_log.pl[i].pid == pid && g_log.pl[i].start == start)
            return i;
    return -1;
}

static void proc_row(sys_t *s, plog_t *pl, proc_t *p, double now)
{
    log_put_time(pl->f, wall_time());
    fprintf(pl->f, ",%.1f,%d,%c", now - pl->t0, (int)p->pid, p->state);
    put_num(pl->f, p->cpu, 1);
    put_num(pl->f, p->mem, 2);
    put_num(pl->f, (double)p->rss / MB, 1);
    fprintf(pl->f, ",%d", p->nthreads);
    put_num(pl->f, p->io_ok ? p->rd_bps / MB : NAN, 3);
    put_num(pl->f, p->io_ok ? p->wr_bps / MB : NAN, 3);
    put_num(pl->f, p->gpu, 1);
    put_num(pl->f, p->gpu >= 0 || p->gpu_mem ? (double)p->gpu_mem / MB : NAN, 1);
    put_num(pl->f, p->nfds >= 0 ? (double)p->nfds : NAN, 0);
    put_num(pl->f, s->cpu.total, 1);
    put_num(pl->f, nn(s->cpu.temp_c), 1);
    fputc('\n', pl->f);
    fflush(pl->f);
    pl->rows++;
    pl->last_row = now;
}

static int plog_open(plog_t *pl, char *err, size_t n)
{
    char name[24], ts[32], base[96];
    str_copy(name, sizeof name, pl->comm);
    for (char *q = name; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '-' && *q != '.')
            *q = '_';
    stamp(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S");
    sfmt(base, sizeof base, "hsm-process-%d-%s-%s", (int)pl->pid, name, ts);
    pl->f = open_new(base, pl->path, sizeof pl->path, err, n);
    if (!pl->f)
        return -1;
    pl->day = log_day_key(wall_time());
    fprintf(pl->f, "time,utc_offset,elapsed_s,pid,state,\"CPU (%% of one core)\",\"Memory (%%)\",\"RSS (MB)\","
                   "threads,\"Disk read (MB/s)\",\"Disk write (MB/s)\",\"GPU (%%)\",\"GPU memory (MB)\","
                   "\"Open files\",\"System CPU (%%)\",\"CPU temperature (C)\"\n");
    return 0;
}

int log_proc_start(sys_t *s, pid_t pid, unsigned long long start, char *err, size_t n)
{
    if (log_proc_find(pid, start) >= 0)
        return 0;
    if (g_log.nplog >= MAX_PLOG) {
        sfmt(err, n, "At most %d processes can be logged at the same time", MAX_PLOG);
        return -1;
    }
    proc_t *p = find_proc(s, pid, start);
    if (!p) {
        sfmt(err, n, "PID %d has already exited", (int)pid);
        return -1;
    }
    plog_t *pl = &g_log.pl[g_log.nplog];
    memset(pl, 0, sizeof *pl);
    pl->pid = pid;
    pl->start = start;
    str_copy(pl->comm, sizeof pl->comm, p->comm);
    if (plog_open(pl, err, n) != 0)
        return -1;
    pl->t0 = now_sec();
    g_log.nplog++;
    proc_row(s, pl, p, pl->t0);
    return 0;
}

static void plog_close(int i)
{
    plog_t *pl = &g_log.pl[i];
    if (pl->f) {
        fflush(pl->f);
        fsync(fileno(pl->f));
        fclose(pl->f);
    }
    memmove(&g_log.pl[i], &g_log.pl[i + 1], (size_t)(g_log.nplog - i - 1) * sizeof *pl);
    g_log.nplog--;
}

void log_proc_stop(int i)
{
    if (i >= 0 && i < g_log.nplog)
        plog_close(i);
}

/* ------------------------------------------------------------------ tick */

/* a new day: carry on in a new file in the new day's folder */
static void new_day(sys_t *s, int today)
{
    char err[PATHBUF + 64], old[PATHBUF];
    (void)s;
    if (g_log.f && g_log.day != today) {
        str_copy(old, sizeof old, g_log.path);
        fflush(g_log.f);
        fsync(fileno(g_log.f));
        fclose(g_log.f);
        g_log.f = NULL;
        if (sys_open(err, sizeof err) != 0)
            note(1, "Logging stopped at midnight: %s", err);
        else
            g_log.rows = 0;
    }
    for (int i = 0; i < g_log.nplog; i++) {
        plog_t *pl = &g_log.pl[i];
        if (pl->day == today)
            continue;
        fflush(pl->f);
        fsync(fileno(pl->f));
        fclose(pl->f);
        pl->f = NULL;
        if (plog_open(pl, err, sizeof err) != 0) {
            note(1, "Stopped logging PID %d at midnight: %s", (int)pl->pid, err);
            plog_close(i--);
        } else {
            pl->rows = 0;
        }
    }
}

void log_tick(sys_t *s)
{
    double now = now_sec(), ev = g_log.every;
    if (g_log.f || g_log.nplog) {
        int today = log_day_key(wall_time()), stale = g_log.f && g_log.day != today;
        for (int i = 0; i < g_log.nplog && !stale; i++)
            stale = g_log.pl[i].day != today;
        if (stale)
            new_day(s, today);
    }
    if (g_log.f && (ev <= 0 || now - g_log.last_row >= ev - 0.05))
        sys_row(s, now);
    for (int i = 0; i < g_log.nplog;) {
        plog_t *pl = &g_log.pl[i];
        proc_t *p = find_proc(s, pl->pid, pl->start);
        if (!p || p->state == 'Z' || p->state == 'X') { /* gone, or a zombie waiting to be reaped */
            note(0, "PID %d (%s) has exited: its log was closed after %lu rows", (int)pl->pid, pl->comm, pl->rows);
            plog_close(i);
            continue;
        }
        if (ev <= 0 || now - pl->last_row >= ev - 0.05)
            proc_row(s, pl, p, now);
        if (ferror(pl->f)) {
            note(1, "Stopped logging PID %d: could not write %s", (int)pl->pid, pl->path);
            plog_close(i);
            continue;
        }
        i++;
    }
    if (now - g_log.last_sync >= SYNC_EVERY) { /* push the data to the disk itself */
        if (g_log.f)
            fsync(fileno(g_log.f));
        for (int i = 0; i < g_log.nplog; i++)
            fsync(fileno(g_log.pl[i].f));
        g_log.last_sync = now;
    }
}

void log_close_all(void)
{
    log_stop();
    while (g_log.nplog)
        plog_close(g_log.nplog - 1);
}

/* ---------------------------------------------------------- housekeeping */

static int our_file(const char *n) /* hsm-....csv */
{
    size_t l = strlen(n);
    return l > 8 && !strncmp(n, "hsm-", 4) && !strcmp(n + l - 4, ".csv");
}

static int day_folder(const char *n) /* 2026-11-12 -> 20261112, else 0 */
{
    if (strlen(n) != 10 || n[4] != '-' || n[7] != '-')
        return 0;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)n[i]))
            return 0;
    int y = atoi(n), m = atoi(n + 5), d = atoi(n + 8);
    return m >= 1 && m <= 12 && d >= 1 && d <= 31 ? y * 10000 + m * 100 + d : 0;
}

static int is_open(const char *dir, const char *sub, const char *name)
{
    char p[PATHBUF];
    if (sub)
        sfmt(p, sizeof p, "%s/%s/%s", dir, sub, name);
    else
        sfmt(p, sizeof p, "%s/%s", dir, name);
    if (g_log.f && !strcmp(p, g_log.path))
        return 1;
    for (int i = 0; i < g_log.nplog; i++)
        if (!strcmp(p, g_log.pl[i].path))
            return 1;
    return 0;
}

/* delete hsm's CSV files in an open folder; all of them, or (cut > 0) those
 * last changed before cut */
static int purge(int dfd, const char *dir, const char *sub, time_t cut)
{
    int fd = dup(dfd), n = 0;
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    rewinddir(d); /* a dup shares the read position with dfd, which may be at the end */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!our_file(e->d_name) || fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
            continue;
        if ((cut && st.st_mtime >= cut) || is_open(dir, sub, e->d_name))
            continue;
        if (unlinkat(dfd, e->d_name, 0) == 0)
            n++;
    }
    closedir(d);
    return n;
}

int log_housekeeping(const char *dir, int keep_days)
{
    if (keep_days <= 0 || !dir || !*dir)
        return 0;
    int who = config_as_user(); /* the logs are the user's, also under sudo */
    if (who < 0)
        return 0;
    int n = 0, dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd >= 0) {
        /* keep today and the keep_days - 1 days before it */
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_mday -= keep_days - 1;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        mktime(&tm);
        int first = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
        int fd = dup(dfd);
        DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
        if (!d && fd >= 0)
            close(fd);
        if (d)
            rewinddir(d);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            struct stat st;
            if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            int day = S_ISDIR(st.st_mode) ? day_folder(e->d_name) : 0;
            if (day && day < first) {
                int sub = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (sub >= 0) {
                    n += purge(sub, dir, e->d_name, 0);
                    close(sub);
                    unlinkat(dfd, e->d_name, AT_REMOVEDIR); /* only works when nothing else is left */
                }
            }
        }
        if (d)
            closedir(d);
        /* logs written before 1.6 sit directly in the folder: go by their age */
        n += purge(dfd, dir, NULL, now - (time_t)keep_days * 86400);
        close(dfd);
    }
    config_as_root(who);
    return n;
}

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
/* serve.c - hsm --serve: the figures over HTTP, for Prometheus / Grafana
 * or a quick look from another machine.
 *
 *   /metrics   Prometheus text format
 *   /json      the same as hsm --json
 *   /          a short page with the links
 *
 * Read-only and tiny: GET and HEAD only, one request per connection, two
 * seconds at most per client. It listens on localhost unless told
 * otherwise; there is no password, so open it to the network only where
 * that is fine. */
#define _GNU_SOURCE
#include "serve.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ 4096

/* ------------------------------------------------------------- metrics */

static FILE *M;

/* label values: backslash, quote and newline escaped */
static void lab(const char *k, const char *v, int more)
{
    fprintf(M, "%s=\"", k);
    for (const char *p = v; *p; p++) {
        if (*p == '\\' || *p == '"')
            fputc('\\', M), fputc(*p, M);
        else if (*p == '\n')
            fputs("\\n", M);
        else
            fputc(*p, M);
    }
    fputs(more ? "\"," : "\"", M);
}

static void help(const char *name, const char *type, const char *text)
{
    fprintf(M, "# HELP hsm_%s %s\n# TYPE hsm_%s %s\n", name, text, name, type);
}

static void val(double v)
{
    if (isnan(v))
        fputs(" NaN\n", M);
    else if (v == floor(v) && fabs(v) < 1e15)
        fprintf(M, " %.0f\n", v);
    else
        fprintf(M, " %.6g\n", v);
}

static void plain(const char *name, double v)
{
    fprintf(M, "hsm_%s", name);
    val(v);
}

/* one label */
static void one(const char *name, const char *k, const char *v, double x)
{
    fprintf(M, "hsm_%s{", name);
    lab(k, v, 0);
    fputc('}', M);
    val(x);
}

/* two labels */
static void two(const char *name, const char *k1, const char *v1, const char *k2, const char *v2, double x)
{
    fprintf(M, "hsm_%s{", name);
    lab(k1, v1, 1);
    lab(k2, v2, 0);
    fputc('}', M);
    val(x);
}

static double nn(double v) /* negative = unknown */
{
    return v < 0 ? NAN : v;
}

void metrics_write(FILE *f, sys_t *s)
{
    M = f;
    char b[32];
    help("up", "gauge", "1 while hsm runs");
    plain("up", 1);
    if (s->mach.ok) { /* an info metric: the labels say it */
        const machine_t *mc = &s->mach;
        char d[16] = "";
        if (mc->bios_date)
            sfmt(d, sizeof d, "%04d-%02d-%02d", mc->bios_date / 10000, mc->bios_date / 100 % 100, mc->bios_date % 100);
        help("machine_info", "gauge", "the machine as its firmware describes it");
        fputs("hsm_machine_info{", M);
        lab("vendor", mc->vendor, 1);
        lab("model", mc->model, 1);
        lab("bios_version", mc->bios_version, 1);
        lab("bios_date", d, 0);
        fputc('}', M);
        val(1);
    }
    char al[64];
    int lvl = health_alert(&s->health, al, sizeof al);
    help("health_status", "gauge", "0 fine, 1 warning, 2 problem (as the screen's badge)");
    plain("health_status", lvl);

    /* CPU */
    help("cpu_usage_percent", "gauge", "CPU busy, all cores");
    plain("cpu_usage_percent", s->cpu.total);
    help("cpu_core_usage_percent", "gauge", "CPU busy, per core");
    for (int i = 0; i < s->cpu.ncpu; i++) {
        sfmt(b, sizeof b, "%d", s->cpu.id[i]);
        one("cpu_core_usage_percent", "core", b, s->cpu.pct[i]);
    }
    help("cpu_clock_mhz", "gauge", "average CPU clock");
    plain("cpu_clock_mhz", s->cpu.mhz_avg > 0 ? s->cpu.mhz_avg : NAN);
    help("cpu_temperature_celsius", "gauge", "CPU temperature");
    plain("cpu_temperature_celsius", nn(s->cpu.temp_c));
    help("load_average", "gauge", "load average");
    one("load_average", "minutes", "1", s->cpu.load[0]);
    one("load_average", "minutes", "5", s->cpu.load[1]);
    one("load_average", "minutes", "15", s->cpu.load[2]);
    help("uptime_seconds", "gauge", "time since boot");
    plain("uptime_seconds", s->cpu.uptime);

    /* memory */
    help("memory_bytes", "gauge", "memory");
    one("memory_bytes", "kind", "total", (double)s->mem.total);
    one("memory_bytes", "kind", "used", (double)s->mem.used);
    one("memory_bytes", "kind", "available", (double)s->mem.avail);
    one("memory_bytes", "kind", "cache", (double)s->mem.buffcache);
    help("swap_bytes", "gauge", "swap");
    one("swap_bytes", "kind", "total", (double)s->mem.swap_total);
    one("swap_bytes", "kind", "used", (double)s->mem.swap_used);
    if (s->mem.nswap) {
        help("swap_device_bytes", "gauge", "each swap device: its size and what's used");
        for (int i = 0; i < s->mem.nswap; i++) {
            two("swap_device_bytes", "device", s->mem.swap[i].name, "kind", "size", (double)s->mem.swap[i].size);
            two("swap_device_bytes", "device", s->mem.swap[i].name, "kind", "used", (double)s->mem.swap[i].used);
        }
    }
    if (s->mem.nzram) {
        help("zram_bytes", "gauge", "compressed RAM: what's stored, compressed to, and the RAM it takes");
        for (int i = 0; i < s->mem.nswap; i++) {
            const swapdev_t *d = &s->mem.swap[i];
            if (!d->zram)
                continue;
            two("zram_bytes", "device", d->name, "kind", "stored", (double)d->orig);
            two("zram_bytes", "device", d->name, "kind", "compressed", (double)d->compr);
            two("zram_bytes", "device", d->name, "kind", "ram", (double)d->ram);
        }
    }
    if (s->mem.oom_ok) {
        help("oom_kills_total", "counter", "processes killed for lack of memory since boot");
        plain("oom_kills_total", (double)s->mem.oom_kill);
    }

    /* GPUs */
    if (s->gpus.n) {
        help("gpu_usage_percent", "gauge", "GPU busy");
        for (int i = 0; i < s->gpus.n; i++) {
            sfmt(b, sizeof b, "%d", i);
            two("gpu_usage_percent", "gpu", b, "name", s->gpus.g[i].name, nn(s->gpus.g[i].util));
        }
        help("gpu_temperature_celsius", "gauge", "GPU temperature");
        for (int i = 0; i < s->gpus.n; i++) {
            sfmt(b, sizeof b, "%d", i);
            one("gpu_temperature_celsius", "gpu", b, nn(s->gpus.g[i].temp_c));
        }
        help("gpu_power_watts", "gauge", "GPU power");
        for (int i = 0; i < s->gpus.n; i++) {
            sfmt(b, sizeof b, "%d", i);
            one("gpu_power_watts", "gpu", b, nn(s->gpus.g[i].power_w));
        }
        int any = 0;
        for (int i = 0; i < s->gpus.n; i++)
            any |= s->gpus.g[i].reasons_ok;
        if (any) {
            help("gpu_slowed_down", "gauge", "1 while the GPU holds its clock back for this reason (heat, power)");
            for (int i = 0; i < s->gpus.n; i++)
                if (s->gpus.g[i].reasons_ok) {
                    sfmt(b, sizeof b, "%d", i);
                    two("gpu_slowed_down", "gpu", b, "reason", "heat", (s->gpus.g[i].slow_now & GS_HEAT) ? 1 : 0);
                    two("gpu_slowed_down", "gpu", b, "reason", "power", (s->gpus.g[i].slow_now & GS_POWER) ? 1 : 0);
                }
        }
        any = 0;
        for (int i = 0; i < s->gpus.n; i++)
            any |= s->gpus.g[i].nv_state != NV_NONE;
        if (any) {
            help("gpu_asleep", "gauge", "1 while the GPU is powered off to save power");
            for (int i = 0; i < s->gpus.n; i++)
                if (s->gpus.g[i].nv_state != NV_NONE) {
                    sfmt(b, sizeof b, "%d", i);
                    one("gpu_asleep", "gpu", b, s->gpus.g[i].nv_state == NV_ASLEEP ? 1 : 0);
                }
            help("gpu_video_percent", "gauge", "video encoder / decoder busy");
            for (int i = 0; i < s->gpus.n; i++)
                if (s->gpus.g[i].nv_state != NV_NONE) {
                    sfmt(b, sizeof b, "%d", i);
                    two("gpu_video_percent", "gpu", b, "engine", "encode", nn(s->gpus.g[i].enc_pct));
                    two("gpu_video_percent", "gpu", b, "engine", "decode", nn(s->gpus.g[i].dec_pct));
                }
            help("gpu_pcie_link", "gauge", "the GPU's PCIe link now: generation and width (lanes)");
            for (int i = 0; i < s->gpus.n; i++)
                if (s->gpus.g[i].nv_state != NV_NONE) {
                    const gpu_t *g = &s->gpus.g[i];
                    sfmt(b, sizeof b, "%d", i);
                    two("gpu_pcie_link", "gpu", b, "what", "generation", g->pcie_gen > 0 ? g->pcie_gen : NAN);
                    two("gpu_pcie_link", "gpu", b, "what", "width", g->pcie_width > 0 ? g->pcie_width : NAN);
                }
        }
    }

    /* power */
    help("power_watts", "gauge", "estimated power draw of the machine");
    plain("power_watts", nn(s->total_w));
    if (s->power.nrapl) {
        help("rapl_watts", "gauge", "power by RAPL zone");
        for (int i = 0; i < s->power.nrapl; i++) {
            sfmt(b, sizeof b, "%s-%d", s->power.z[i].name, i);
            one("rapl_watts", "zone", b, s->power.z[i].readable ? s->power.z[i].watts : NAN);
        }
    }
    if (s->power.nbat) {
        help("battery_charge_percent", "gauge", "battery charge");
        for (int i = 0; i < s->power.nbat; i++)
            one("battery_charge_percent", "battery", s->power.bat[i].name, nn(s->power.bat[i].capacity));
        help("battery_health_percent", "gauge", "full charge now against new");
        for (int i = 0; i < s->power.nbat; i++)
            one("battery_health_percent", "battery", s->power.bat[i].name, nn(s->power.bat[i].health));
    }

    /* sensors */
    static const char *const ty[] = { "temperature", "fan", "power", "voltage" };
    static const char *const un[] = { "celsius", "rpm", "watts", "volts" };
    for (int t = 0; t < 4; t++) {
        int any = 0;
        for (int i = 0; i < s->sens.n; i++) {
            const sensor_t *x = &s->sens.s[i];
            if (x->type != t || x->hidden)
                continue;
            char name[48];
            sfmt(name, sizeof name, "sensor_%s_%s", ty[t], un[t]);
            if (!any) {
                char h[64];
                sfmt(h, sizeof h, "hwmon %s sensors", ty[t]);
                help(name, "gauge", h);
                any = 1;
            }
            fprintf(M, "hsm_%s{", name);
            lab("chip", x->chip, 1);
            lab("label", x->label, x->name[0] ? 1 : 0);
            if (x->name[0])
                lab("name", x->name, 0);
            fputc('}', M);
            val(x->ok ? x->value : NAN);
        }
    }

    /* disks */
    if (s->disks.n) {
        help("disk_read_bytes_total", "counter", "bytes read since boot");
        for (int i = 0; i < s->disks.n; i++)
            one("disk_read_bytes_total", "disk", s->disks.d[i].name, (double)s->disks.d[i].rd_total);
        help("disk_written_bytes_total", "counter", "bytes written since boot");
        for (int i = 0; i < s->disks.n; i++)
            one("disk_written_bytes_total", "disk", s->disks.d[i].name, (double)s->disks.d[i].wr_total);
        help("disk_busy_percent", "gauge", "share of time the disk was busy");
        for (int i = 0; i < s->disks.n; i++)
            one("disk_busy_percent", "disk", s->disks.d[i].name, s->disks.d[i].util);
        help("disk_latency_milliseconds", "gauge", "time per request");
        for (int i = 0; i < s->disks.n; i++) {
            two("disk_latency_milliseconds", "disk", s->disks.d[i].name, "op", "read", s->disks.d[i].rd_lat);
            two("disk_latency_milliseconds", "disk", s->disks.d[i].name, "op", "write", s->disks.d[i].wr_lat);
        }
    }
    if (s->disks.nfs) {
        help("filesystem_size_bytes", "gauge", "file system size");
        for (int i = 0; i < s->disks.nfs; i++)
            one("filesystem_size_bytes", "mount", s->disks.fs[i].mount, (double)s->disks.fs[i].total);
        help("filesystem_free_bytes", "gauge", "space left for normal users");
        for (int i = 0; i < s->disks.nfs; i++)
            one("filesystem_free_bytes", "mount", s->disks.fs[i].mount, (double)s->disks.fs[i].avail);
        help("filesystem_readonly", "gauge", "1 when mounted (or turned) read-only");
        for (int i = 0; i < s->disks.nfs; i++)
            one("filesystem_readonly", "mount", s->disks.fs[i].mount, s->disks.fs[i].readonly);
    }

    /* network */
    if (s->net.n) {
        help("network_receive_bytes_total", "counter", "bytes received since boot");
        for (int i = 0; i < s->net.n; i++)
            one("network_receive_bytes_total", "interface", s->net.i[i].name, (double)s->net.i[i].rx_total);
        help("network_transmit_bytes_total", "counter", "bytes sent since boot");
        for (int i = 0; i < s->net.n; i++)
            one("network_transmit_bytes_total", "interface", s->net.i[i].name, (double)s->net.i[i].tx_total);
        help("network_up", "gauge", "1 while the link is up");
        for (int i = 0; i < s->net.n; i++)
            one("network_up", "interface", s->net.i[i].name, s->net.i[i].up);
        help("network_link_drops_total", "counter", "times the link was lost since boot");
        for (int i = 0; i < s->net.n; i++)
            if (s->net.i[i].link_downs >= 0)
                one("network_link_drops_total", "interface", s->net.i[i].name, (double)s->net.i[i].link_downs);
        int wl = 0; /* Wi-Fi, one metric at a time */
        for (int i = 0; i < s->net.n; i++)
            wl |= s->net.i[i].wireless && s->net.i[i].wifi.ok;
        if (wl) {
            help("wifi_signal_dbm", "gauge", "the Wi-Fi signal");
            for (int i = 0; i < s->net.n; i++)
                if (s->net.i[i].wifi.ok && s->net.i[i].quality >= 0)
                    one("wifi_signal_dbm", "interface", s->net.i[i].name, s->net.i[i].signal_dbm);
            help("wifi_link_mbps", "gauge", "the Wi-Fi link speed, each way");
            for (int i = 0; i < s->net.n; i++) {
                const netif_t *nf = &s->net.i[i];
                if (nf->wifi.ok && nf->wifi.tx_mbps > 0)
                    two("wifi_link_mbps", "interface", nf->name, "direction", "tx", nf->wifi.tx_mbps);
                if (nf->wifi.ok && nf->wifi.rx_mbps > 0)
                    two("wifi_link_mbps", "interface", nf->name, "direction", "rx", nf->wifi.rx_mbps);
            }
            help("wifi_frequency_mhz", "gauge", "the Wi-Fi channel's frequency (2.4, 5 or 6 GHz band)");
            for (int i = 0; i < s->net.n; i++)
                if (s->net.i[i].wifi.ok)
                    one("wifi_frequency_mhz", "interface", s->net.i[i].name, s->net.i[i].wifi.freq);
        }
    }
    if (s->net.q.on && !s->net.q.err) { /* the network check */
        static const char *const nm[NQ_COUNT] = { "router", "internet" };
        help("ping_seconds", "gauge", "the latest ping's delay (NaN: lost)");
        for (int k = 0; k < NQ_COUNT; k++)
            if (s->net.q.t[k].addr[0])
                one("ping_seconds", "target", nm[k], s->net.q.t[k].rtt_ms >= 0 ? s->net.q.t[k].rtt_ms / 1000.0 : NAN);
        help("ping_loss_ratio", "gauge", "pings lost over the last minute");
        for (int k = 0; k < NQ_COUNT; k++)
            if (s->net.q.t[k].addr[0])
                one("ping_loss_ratio", "target", nm[k], s->net.q.t[k].loss_pct >= 0 ? s->net.q.t[k].loss_pct / 100.0 : NAN);
    }

    /* health */
    const health_t *h = &s->health;
    static const char *const pn[3] = { "cpu", "memory", "io" };
    if (h->psi[0].ok) {
        help("pressure_percent", "gauge", "share of time tasks waited for a resource, 10 s average");
        for (int i = 0; i < 3; i++) {
            two("pressure_percent", "resource", pn[i], "kind", "some", h->psi[i].avg10);
            if (h->psi[i].has_full)
                two("pressure_percent", "resource", pn[i], "kind", "full", h->psi[i].favg10);
        }
    }
    if (h->thr_ok) {
        help("throttle_events_total", "counter", "thermal throttling events since boot");
        plain("throttle_events_total", (double)(h->thr_count + h->core_count));
    }
    const cpulim_t *L = &s->power.lim;
    if (L->ok) {
        help("cpu_power_limit_watts", "gauge", "how much the CPU package may draw (the lower of the CPU's and the firmware's)");
        if (L->eff_long_w > 0)
            one("cpu_power_limit_watts", "limit", "sustained", L->eff_long_w);
        if (L->eff_short_w > 0)
            one("cpu_power_limit_watts", "limit", "boost", L->eff_short_w);
        if (h->pl_known) {
            help("cpu_power_limited", "gauge", "1 while the power limit, not heat, holds the CPU's clock back");
            plain("cpu_power_limited", h->pl_now ? 1 : 0);
        }
    }
    if (h->aer_ok) {
        help("pcie_errors_total", "counter", "PCIe errors since boot");
        one("pcie_errors_total", "severity", "corrected", (double)h->pci_cor);
        one("pcie_errors_total", "severity", "nonfatal", (double)h->pci_nonfatal);
        one("pcie_errors_total", "severity", "fatal", (double)h->pci_fatal);
    }
    if (s->usb.ok) {
        help("usb_devices", "gauge", "USB devices plugged in (hubs included)");
        plain("usb_devices", s->usb.n);
        help("usb_slow_devices", "gauge", "USB 3 drives and network adapters running at USB 2 speed");
        plain("usb_slow_devices", s->usb.nslow);
    }
    help("freezes_total", "counter", "times the whole system stood still for 2 s or more, since hsm started");
    plain("freezes_total", h->freezes);
    help("runaway_processes", "gauge", "processes that have kept a whole core busy for 15 minutes or more without a break");
    plain("runaway_processes", runaway_count());
    help("frozen_seconds_total", "counter", "seconds the system stood still, since hsm started");
    plain("frozen_seconds_total", h->frozen_total);
    if (h->kmsg == 1) {
        help("kernel_errors_total", "counter", "kernel log errors since boot");
        plain("kernel_errors_total", h->kmsg_err);
    }
    int any = 0;
    for (int i = 0; i < h->nnvme; i++) {
        const nvme_t *v = &h->nvme[i];
        if (v->state != 1)
            continue;
        if (!any++)
            help("drive_wear_percent", "gauge", "share of the rated life used");
        one("drive_wear_percent", "drive", v->name, v->used);
    }
    for (int i = 0; i < h->nsata; i++) {
        const sata_t *d = &h->sata[i];
        if (d->state != 1 || d->wear < 0)
            continue;
        if (!any++)
            help("drive_wear_percent", "gauge", "share of the rated life used");
        one("drive_wear_percent", "drive", d->name, d->wear);
    }
    any = 0;
    for (int i = 0; i < h->nnvme; i++) {
        const nvme_t *v = &h->nvme[i];
        if (v->state != 1)
            continue;
        if (!any++)
            help("drive_media_errors_total", "counter", "errors the drive could not correct, in its life");
        one("drive_media_errors_total", "drive", v->name, (double)v->media_err);
    }
    for (int i = 0; i < h->nsata; i++) {
        const sata_t *d = &h->sata[i];
        if (d->state != 1 || d->uncorrect < 0)
            continue;
        if (!any++)
            help("drive_media_errors_total", "counter", "errors the drive could not correct, in its life");
        one("drive_media_errors_total", "drive", d->name, (double)d->uncorrect);
    }
    any = 0;
    for (int i = 0; i < h->nnvme; i++) { /* the drive's own thermal management */
        const nvme_t *v = &h->nvme[i];
        if (v->state != 1)
            continue;
        if (!any++)
            help("drive_heat_slowdowns_total", "counter", "times the drive slowed itself down from heat, in its life");
        two("drive_heat_slowdowns_total", "drive", v->name, "kind", "light", (double)v->heat_n[0]);
        two("drive_heat_slowdowns_total", "drive", v->name, "kind", "hard", (double)v->heat_n[1]);
    }
    any = 0;
    for (int i = 0; i < h->nnvme; i++) {
        const nvme_t *v = &h->nvme[i];
        if (v->state != 1)
            continue;
        if (!any++)
            help("drive_heat_slowed_seconds_total", "counter", "seconds the drive ran slowed down from heat, in its life");
        two("drive_heat_slowed_seconds_total", "drive", v->name, "kind", "light", (double)v->heat_s[0]);
        two("drive_heat_slowed_seconds_total", "drive", v->name, "kind", "hard", (double)v->heat_s[1]);
    }
    any = 0;
    for (int i = 0; i < h->nsata; i++) {
        const sata_t *d = &h->sata[i];
        if (d->state != 1 || d->realloc < 0)
            continue;
        if (!any++)
            help("drive_bad_sectors", "gauge", "reallocated plus pending sectors");
        one("drive_bad_sectors", "drive", d->name, (double)(d->realloc + MAX(d->pending, 0)));
    }
}

/* --------------------------------------------------------------- server */

int serve_open(const char *spec, char *err, size_t n, char *shown, size_t sn)
{
    char host[128] = "127.0.0.1", port[16];
    const char *colon = strrchr(spec, ':');
    if (colon) { /* "0.0.0.0:9101", "[::]:9101", "localhost:9101" */
        size_t hl = (size_t)(colon - spec);
        if (hl >= sizeof host) {
            sfmt(err, n, "%s: address too long", spec);
            return -1;
        }
        memcpy(host, spec, hl);
        host[hl] = '\0';
        if (host[0] == '[' && hl >= 2 && host[hl - 1] == ']') {
            memmove(host, host + 1, hl - 2);
            host[hl - 2] = '\0';
        }
        str_copy(port, sizeof port, colon + 1);
    } else {
        str_copy(port, sizeof port, spec);
    }
    char *end;
    long pv = strtol(port, &end, 10);
    if (*end || pv < 1 || pv > 65535) {
        sfmt(err, n, "'%s' is not a port (1-65535)", port);
        return -1;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    int r = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (r != 0) {
        sfmt(err, n, "%s: %s", host, gai_strerror(r));
        return -1;
    }
    int fd = -1, e = 0;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) {
            e = errno;
            continue;
        }
        int one_ = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one_, sizeof one_);
        if (bind(fd, a->ai_addr, a->ai_addrlen) != 0 || listen(fd, 16) != 0) {
            e = errno;
            close(fd);
            fd = -1;
            continue;
        }
        char hb[NI_MAXHOST];
        if (getnameinfo(a->ai_addr, a->ai_addrlen, hb, sizeof hb, NULL, 0, NI_NUMERICHOST) == 0)
            sfmt(shown, sn, strchr(hb, ':') ? "[%s]:%ld" : "%s:%ld", hb, pv);
        else
            sfmt(shown, sn, "%s:%ld", host, pv);
    }
    freeaddrinfo(res);
    if (fd < 0)
        sfmt(err, n, "can't listen on %s:%s: %s", host, port, strerror(e));
    return fd;
}

static void send_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return;
        }
        b += w;
        n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const char *body, size_t len, int head)
{
    const char *why = code == 200 ? "OK" : code == 405 ? "Method Not Allowed" : "Not Found";
    char h[512];
    int hl = sfmt(h, sizeof h,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n",
                  code, why, type, len,
                  code == 405 ? "Allow: GET, HEAD\r\n" : "");
    send_all(fd, h, (size_t)MIN(hl, (int)sizeof h - 1));
    if (!head && len)
        send_all(fd, body, len);
}

static void index_page(FILE *f, sys_t *s)
{
    fprintf(f,
            "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>hsm on %s</title></head>\n"
            "<body style=\"font-family: monospace\">\n<h1>hsm %s on %s</h1>\n<ul>\n"
            "<li><a href=\"/metrics\">/metrics</a>: Prometheus text format</li>\n"
            "<li><a href=\"/json\">/json</a>: everything, as <code>hsm --json</code></li>\n</ul>\n",
            s->host, APP_VERSION, s->host);
    char al[64];
    int lvl = health_alert(&s->health, al, sizeof al);
    char t[32] = "";
    if (s->cpu.temp_c >= 0)
        sfmt(t, sizeof t, ", %.0f &deg;C", s->cpu.temp_c);
    fprintf(f, "<p>CPU %.1f%%%s &middot; memory %.0f%% used &middot; health: %s</p>\n</body></html>\n", s->cpu.total,
            t, s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : 0, lvl ? al : "fine");
}

static void handle(int lfd, sys_t *s)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return;
    struct timeval tv = { 2, 0 }; /* a slow client doesn't hold up the sampling for long */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[MAX_REQ + 1];
    size_t got = 0;
    double until = now_sec() + 2.0;
    while (got < MAX_REQ && now_sec() < until) {
        ssize_t r = recv(fd, req + got, MAX_REQ - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        got += (size_t)r;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[got] = '\0';
    char method[8] = "", path[256] = "";
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        close(fd);
        return;
    }
    path[strcspn(path, "?#")] = '\0';
    int head = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && !head) {
        reply(fd, 405, "text/plain; charset=utf-8", "GET or HEAD only\n", 17, 0);
        close(fd);
        return;
    }
    char *body = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&body, &len);
    if (!f) {
        close(fd);
        return;
    }
    const char *type = "text/plain; charset=utf-8";
    int code = 200;
    if (!strcmp(path, "/metrics")) {
        metrics_write(f, s);
        type = "text/plain; version=0.0.4; charset=utf-8";
    } else if (!strcmp(path, "/json")) {
        json_write(f, s, -1);
        type = "application/json";
    } else if (!strcmp(path, "/")) {
        index_page(f, s);
        type = "text/html; charset=utf-8";
    } else {
        code = 404;
        fputs("Not found: try /metrics or /json\n", f);
    }
    fclose(f);
    reply(fd, code, type, body, len, head);
    free(body);
    close(fd);
}

void serve_wait(int lfd, double seconds, sys_t *s, volatile sig_atomic_t *quit)
{
    double until = now_sec() + seconds;
    while (!*quit) {
        double left = until - now_sec();
        if (left <= 0)
            break;
        struct pollfd p = { lfd, POLLIN, 0 };
        int r = poll(&p, 1, (int)(left * 1000) + 1);
        if (r > 0 && (p.revents & POLLIN))
            handle(lfd, s);
    }
}

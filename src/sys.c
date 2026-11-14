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

/* sys.c - ties all collectors together and keeps the graph history */
#define _GNU_SOURCE
#include "collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static void read_os_name(char *out, size_t n)
{
    char buf[4096];
    str_copy(out, n, "Linux");
    if (read_file("/etc/os-release", buf, sizeof buf) <= 0 && read_file("/usr/lib/os-release", buf, sizeof buf) <= 0)
        return;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (strncmp(line, "PRETTY_NAME=", 12))
            continue;
        char *v = line + 12;
        size_t l = strlen(v);
        if (l >= 2 && (v[0] == '"' || v[0] == '\'') && v[l - 1] == v[0]) {
            v[l - 1] = '\0';
            v++;
        }
        str_copy(out, n, v);
        return;
    }
}

/* a DMI field, or "" for the placeholders firmware leaves in ("To Be Filled By O.E.M.") */
static void dmi(const char *name, char *out, size_t n)
{
    static const char *const junk[] = { "to be filled by o.e.m.", "default string", "system product name",
                                        "system version", "system manufacturer", "not applicable", "not specified",
                                        "none", "n/a", "type1productconfigid", "0123456789", "123456789", "x.x",
                                        "invalid", "oem", "o.e.m.", "unknown", "sku", "base board product name" };
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/class/dmi/id/%s", g_sysfs, name);
    out[0] = '\0';
    if (read_line(p, out, n))
        return;
    char *a = out;
    while (*a == ' ')
        a++;
    memmove(out, a, strlen(a) + 1);
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ')
        out[--l] = '\0';
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        if (!strcasecmp(out, junk[i]))
            out[0] = '\0';
}

int machine_desktop(const machine_t *m)
{
    static const int desk[] = { 3, 4, 5, 6, 7, 13, 15, 16, 24, 35, 36 };
    for (size_t i = 0; i < sizeof desk / sizeof desk[0]; i++)
        if (m->chassis == desk[i])
            return 1;
    return 0;
}

static void read_machine(machine_t *m)
{
    char name[96], ver[64], fam[96], bn[64], bv[64], date[24];
    memset(m, 0, sizeof *m);
    dmi("sys_vendor", m->vendor, sizeof m->vendor);
    dmi("product_name", name, sizeof name);
    dmi("product_version", ver, sizeof ver);
    dmi("product_family", fam, sizeof fam);
    /* Lenovo puts a type code in the name ("10RRS0R700") and the model in the version */
    if (!strcasecmp(m->vendor, "LENOVO") && (ver[0] || fam[0])) {
        str_copy(m->model, sizeof m->model, ver[0] ? ver : fam);
        str_copy(m->type, sizeof m->type, name);
    } else {
        str_copy(m->model, sizeof m->model, name[0] ? name : fam);
    }
    dmi("board_vendor", bv, sizeof bv);
    dmi("board_name", bn, sizeof bn);
    /* a desktop's name is its board's ("System Product Name"; MSI's "MS-7C91" inside "MAG B550 TOMAHAWK (MS-7C91)") */
    if (bn[0] && (!m->model[0] || (strlen(m->model) >= 4 && strstr(bn, m->model)))) {
        str_copy(m->model, sizeof m->model, bn);
        if (!m->vendor[0])
            str_copy(m->vendor, sizeof m->vendor, bv);
    } else if (bn[0]) {
        sfmt(m->board, sizeof m->board, "%s%s%s", bv[0] && strcasecmp(bv, m->vendor) ? bv : "",
             bv[0] && strcasecmp(bv, m->vendor) ? " " : "", bn);
    }
    char ct[16];
    dmi("chassis_type", ct, sizeof ct);
    m->chassis = atoi(ct);
    dmi("bios_vendor", m->bios_vendor, sizeof m->bios_vendor);
    dmi("bios_version", m->bios_version, sizeof m->bios_version);
    dmi("bios_date", date, sizeof date);
    int mo, d, y;
    if (sscanf(date, "%d/%d/%d", &mo, &d, &y) == 3 && y > 1980 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31)
        m->bios_date = y * 10000 + mo * 100 + d;
    if (!m->model[0]) { /* ARM boards: no DMI, but a device tree ("Raspberry Pi 4 Model B Rev 1.4") */
        char p[PATHBUF], b[128];
        sfmt(p, sizeof p, "%s/firmware/devicetree/base/model", g_sysfs);
        ssize_t n = read_file(p, b, sizeof b);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                if ((unsigned char)b[i] < 32)
                    b[i] = b[i] ? ' ' : '\0';
            str_copy(m->model, sizeof m->model, b);
            for (size_t l = strlen(m->model); l && m->model[l - 1] == ' ';)
                m->model[--l] = '\0';
        }
    }
    m->ok = m->model[0] || m->vendor[0] || m->bios_version[0];
}

void sys_init(sys_t *s)
{
    struct utsname u;
    memset(s, 0, sizeof *s);
    if (uname(&u) == 0) {
        str_copy(s->host, sizeof s->host, u.nodename);
        str_copy(s->kernel, sizeof s->kernel, u.release);
        str_copy(s->arch, sizeof s->arch, u.machine);
    }
    read_os_name(s->os, sizeof s->os);
    read_machine(&s->mach);
    s->total_w = -1;
    s->total_w_src = "";
    s->total_why = "";

    cpu_init(&s->cpu);
    procs_init(&s->procs);
    sensors_init(&s->sens);
    gpus_init(&s->gpus);
    power_init(&s->power);
    health_init(&s->health);
    cpu_power_sample(&s->cpu);
    sys_sample(s); /* primes every counter; rates appear from the next sample */
}

static void estimate_total_power(sys_t *s)
{
    power_t *p = &s->power;
    double bat = 0;
    int discharging = 0;
    for (int i = 0; i < p->nbat; i++)
        if (!strcmp(p->bat[i].status, "Discharging") && p->bat[i].watts > 0) {
            bat += p->bat[i].watts;
            discharging = 1;
        }
    s->total_why = "";
    s->pw_partial = 0;
    if (discharging) { /* battery drain = whole laptop */
        s->total_w = bat;
        s->total_w_src = "battery";
        return;
    }
    /* the platform domain on recent Intel laptops - unless the firmware left
     * it unconnected, which shows as less than the CPU package inside it */
    if (p->psys_w >= 0 && !p->psys_bad && (p->pkg_w < 0 || p->psys_w >= 0.9 * p->pkg_w)) {
        s->total_w = p->psys_w;
        s->total_w_src = "platform";
        return;
    }
    double gpu = 0;
    int ngpu = 0;
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        /* an integrated GPU is already inside the CPU package figure */
        if (g->power_w >= 0 && !g->integrated) {
            gpu += g->power_w;
            ngpu++;
        }
    }
    if (p->pkg_w < 0) {
        /* without the CPU there is no total: the GPUs alone would pass for
         * one (47 W "total" while the CPU drew another 40) */
        s->total_w = -1;
        s->total_w_src = "";
        s->total_why = p->rapl_denied ? "needs root" : "";
        s->pw_partial = ngpu > 0;
        s->part_w = gpu;
        return;
    }
    s->total_w = p->pkg_w + (p->dram_w > 0 ? p->dram_w : 0) + gpu;
    s->total_w_src = ngpu ? "CPU+GPU" : "CPU";
}

/* ------------------------------------------------- readings that hang
 *
 * hsm reads everything in turn, so one slow reading holds up the whole
 * sample and its screen. Each part is timed; the slowest one, if it took a
 * second or more, becomes an event naming it - one sample later, once the
 * freeze watchdog has had its say: if the whole system stood still, that
 * freeze is the news, not the reading that happened to be waiting. */
#define SLOW_READ 1.0

static void timed(char *worst, size_t n, double *wd, double t0, const char *what)
{
    double d = now_sec() - t0;
    if (d > *wd) {
        *wd = d;
        str_copy(worst, n, what);
    }
}

static void slow_report(sys_t *s, const char *worst, double wd)
{
    health_t *h = &s->health;
    if (s->slow_what[0]) { /* last sample's: no freeze or suspend reported since? */
        if (h->freezes == s->slow_frz && h->suspends == s->slow_susp) {
            double now = now_sec();
            int k, old = 0;
            for (k = 0; k < 8 && strcmp(s->slow_seen[k].what, s->slow_what); k++)
                if (s->slow_seen[k].t < s->slow_seen[old].t)
                    old = k;
            if (k == 8) { /* new: the least recent slot */
                k = old;
                str_copy(s->slow_seen[k].what, sizeof s->slow_seen[k].what, s->slow_what);
                s->slow_seen[k].t = -1e9;
            }
            if (now - s->slow_seen[k].t >= 600) {
                s->slow_seen[k].t = now;
                health_event(h, EV_WARN, "slow", "Reading %s took %.1f s; hsm waited for it", s->slow_what,
                             s->slow_dur);
            }
        }
        s->slow_what[0] = '\0';
    }
    if (wd >= SLOW_READ) {
        str_copy(s->slow_what, sizeof s->slow_what, worst);
        s->slow_dur = wd;
        s->slow_frz = h->freezes;
        s->slow_susp = h->suspends;
    }
}

void sys_sample(sys_t *s)
{
    double t = now_sec();
    double dt = s->samples ? t - s->t_last : 0;
    s->t_last = t;
    s->dt = dt;

    char worst[96] = "", what[96];
    double wd = 0, t0 = now_sec();
    cpu_sample(&s->cpu, dt);
    timed(worst, sizeof worst, &wd, t0, "the CPU figures");
    t0 = now_sec();
    mem_sample(&s->mem, dt);
    timed(worst, sizeof worst, &wd, t0, "the memory figures");
    t0 = now_sec();
    disks_sample(&s->disks, dt);
    timed(worst, sizeof worst, &wd, t0, "the disk counters");
    t0 = now_sec();
    net_sample(&s->net, dt);
    netq_read(&s->net.q);
    timed(worst, sizeof worst, &wd, t0, "the network counters");
    t0 = now_sec();
    procs_sample(&s->procs, dt, s->mem.total);
    if (s->procs.slow_s >= SLOW_READ) {
        const char *comm = "";
        for (int i = 0; i < s->procs.n; i++)
            if (s->procs.p[i].pid == s->procs.slow_pid)
                comm = s->procs.p[i].comm;
        sfmt(what, sizeof what, "process %d%s%s%s", (int)s->procs.slow_pid, comm[0] ? " (" : "", comm, comm[0] ? ")" : "");
    } else {
        str_copy(what, sizeof what, "the process list");
    }
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    sensors_sample(&s->sens);
    if (s->sens.slow[0]) {
        char chip[32];
        sensors_chip_name(s->sens.slow, chip, sizeof chip);
        sfmt(what, sizeof what, "the %s sensors", chip);
    } else {
        str_copy(what, sizeof what, "the sensors");
    }
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    gpus_sample(&s->gpus, dt);
    sfmt(what, sizeof what, "the %s", s->gpus.slow[0] ? s->gpus.slow : "GPUs");
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    gpus_proc_sample(&s->gpus, &s->procs);
    timed(worst, sizeof worst, &wd, t0, "the GPU use of processes (NVML)");
    t0 = now_sec();
    power_sample(&s->power, dt);
    timed(worst, sizeof worst, &wd, t0, s->power.slow[0] ? s->power.slow : "the power figures");
    s->cpu.temp_c = sensors_cpu_temp(&s->sens);

    if (t >= s->next_slow) { /* things that change slowly */
        t0 = now_sec();
        fs_sample(&s->disks);
        timed(worst, sizeof worst, &wd, t0, "the file systems");
        t0 = now_sec();
        net_addrs(&s->net);
        cpu_power_sample(&s->cpu);
        timed(worst, sizeof worst, &wd, t0, "the network addresses and CPU settings");
        s->next_slow = t + 5.0;
    }
    t0 = now_sec();
    health_sample(s);
    timed(worst, sizeof worst, &wd, t0, s->health.slow[0] ? s->health.slow : "the health checks");
    t0 = now_sec();
    usb_sample(s);
    timed(worst, sizeof worst, &wd, t0, "the USB devices");
    t0 = now_sec();
    svcs_sample(&s->svc);
    timed(worst, sizeof worst, &wd, t0, "the services (cgroups)");
    slow_report(s, worst, wd);

    /* Intel iGPU power = RAPL "uncore" (power plane 1) on client CPUs */
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (!strcmp(g->vendor, "Intel") && g->integrated && g->power_w < 0 && s->power.uncore_w >= 0)
            g->power_w = s->power.uncore_w;
    }
    estimate_total_power(s);

    if (s->samples) {
        hist_push(&s->h_cpu, s->cpu.total);
        hist_push(&s->h_mem, s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : 0);
        for (int i = 0; i < s->gpus.n; i++)
            hist_push(&s->h_gpu[i], s->gpus.g[i].util > 0 ? s->gpus.g[i].util : 0);
        hist_push(&s->h_dr, s->disks.rd_bps);
        hist_push(&s->h_dw, s->disks.wr_bps);
        hist_push(&s->h_rx, s->net.rx_bps);
        hist_push(&s->h_tx, s->net.tx_bps);
        hist_push(&s->h_pw, s->total_w > 0 ? s->total_w : s->pw_partial ? s->part_w : 0);
        hist_push(&s->h_temp, s->cpu.temp_c > 0 ? s->cpu.temp_c : 0);
    }
    s->samples++;
}

void sys_free(sys_t *s)
{
    procs_free(&s->procs);
    gpus_close(&s->gpus);
    health_close(&s->health);
    svcs_free(&s->svc);
}

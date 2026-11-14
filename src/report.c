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

/* report.c - one-shot plain-text snapshot (hsm --once), handy for scripts,
 * bug reports and ssh sessions without a full terminal */
#define _GNU_SOURCE
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int svc_by_mem(const void *a, const void *b)
{
    const svc_t *p = *(svc_t *const *)a, *q = *(svc_t *const *)b;
    return (p->mem < q->mem) - (p->mem > q->mem);
}

static void report_health(sys_t *s)
{
    health_t *h = &s->health;
    char a[32], b[32];
    printf("\nHealth\n");
    static const char *const pn[3] = { "CPU", "memory", "I/O" };
    if (h->psi[0].ok) {
        printf("  pressure    ");
        for (int i = 0; i < 3; i++) {
            psi_t *p = &h->psi[i];
            printf("%s %.1f%%", pn[i], p->some);
            if (i && p->has_full)
                printf(" (full %.1f%%)", p->full);
            printf(i < 2 ? ",  " : "");
        }
        printf("   (share of time tasks had to wait)\n");
    } else {
        printf("  pressure    not available (needs Linux 4.20+ with PSI enabled)\n");
    }
    if (h->thr_ok) {
        unsigned long long now = h->thr_count + h->core_count - h->thr_start;
        printf("  throttling  %llu times since boot (%.1f s in total)%s%s\n", h->thr_count + h->core_count,
               h->thr_ms / 1000.0, now ? ", throttled now" : "", h->thr_pct > 0 ? "" : "");
    } else {
        printf("  throttling  not reported by this CPU\n");
    }
    if (s->mem.oom_ok)
        printf("  OOM kills   %llu since boot\n", s->mem.oom_kill);
    if (h->aer_ok)
        printf("  PCIe errors %llu corrected, %llu uncorrected since boot\n", h->pci_cor, h->pci_nonfatal + h->pci_fatal);
    else
        printf("  PCIe errors not reported (no AER on this machine)\n");
    for (int i = 0; i < h->npci; i++)
        printf("    %s  %s: %llu corrected, %llu non-fatal, %llu fatal\n", h->pci[i].dev, h->pci[i].name, h->pci[i].cor,
               h->pci[i].nonfatal, h->pci[i].fatal);
    if (h->files_max) {
        fmt_si((double)h->files, a, sizeof a);
        printf("  open files  %s handles in use\n", a);
    }
    for (int i = 0; i < h->nnvme; i++) {
        nvme_t *v = &h->nvme[i];
        printf("  %-11s %s: ", v->name, v->model[0] ? v->model : "NVMe drive");
        if (v->state == 1) {
            fmt_bytes(v->written_b, a, sizeof a);
            fmt_bytes(v->read_b, b, sizeof b);
            printf("%s, %d%% worn, spare %d%%, %llu unsafe shutdowns, %llu media errors, %llu h, %s written, %s read\n",
                   v->crit ? "WARNING" : "OK", v->used, v->spare, v->unsafe, v->media_err, v->hours, a, b);
            if (v->heat_n[0] || v->heat_n[1] || v->heat_s[0] || v->heat_s[1])
                printf("  %-11s heat slowdowns since new: %llu light (%llu s), %llu hard (%llu s)\n", "", v->heat_n[0],
                       v->heat_s[0], v->heat_n[1], v->heat_s[1]);
        } else {
            printf("%s\n", v->state == -1 ? "health needs root (sudo)" : "health not available");
        }
    }
    for (int i = 0; i < h->nsata; i++) {
        sata_t *d = &h->sata[i];
        char sum[200];
        int lvl;
        sata_summary(d, sum, sizeof sum, &lvl, ", ", "C");
        printf("  %-11s %s (SATA %s): %s\n", d->name, d->model[0] ? d->model : "drive",
               d->rotational ? "hard disk" : "SSD", sum);
    }
    if (h->kmsg == 1)
        printf("  kernel log  %u errors, %u warnings since boot\n", h->kmsg_err, h->kmsg_warn);
    else
        printf("  kernel log  %s\n", h->kmsg == -1 ? "needs root (sudo)" : "not available");
    int shown = 0;
    for (int i = 0; i < h->nev && shown < 8; i++) {
        const event_t *e = health_event_at(h, i);
        if (e->level == EV_INFO)
            continue;
        if (!shown++)
            printf("  recent events (newest first)\n");
        char ts[32];
        time_t t = (time_t)e->t;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
        printf("    %s  %-8s %s%s\n", ts, e->cat, e->level == EV_ERR ? "! " : "", e->text);
        char ex[400];
        if (e->level != EV_INFO && event_explain(e->cat, e->text, ex, sizeof ex))
            printf("              %s\n", ex);
    }
}

static int by_cpu(const void *a, const void *b)
{
    const proc_t *p = *(proc_t *const *)a, *q = *(proc_t *const *)b;
    int r = (p->cpu < q->cpu) - (p->cpu > q->cpu);
    return r ? r : (p->rss < q->rss) - (p->rss > q->rss);
}

void report_print(sys_t *s, int nproc)
{
    char a[32], b[32], c[32];
    cpu_t *cp = &s->cpu;
    mem_t *m = &s->mem;

    fmt_duration(cp->uptime, a, sizeof a);
    printf("%s %s  %s | %s | kernel %s (%s) | up %s\n", APP_NAME, APP_VERSION, s->host, s->os, s->kernel, s->arch, a);
    const machine_t *mc = &s->mach;
    if (mc->ok) {
        printf("machine  %s%s%s%s", mc->model[0] ? mc->model : "?", mc->type[0] ? " (" : "", mc->type, mc->type[0] ? ")" : "");
        if (mc->vendor[0])
            printf(" by %s", mc->vendor);
        if (mc->bios_version[0]) {
            printf(" | BIOS %s", mc->bios_version);
            if (mc->bios_date)
                printf(" (%04d-%02d-%02d)", mc->bios_date / 10000, mc->bios_date / 100 % 100, mc->bios_date % 100);
        }
        printf("\n");
    }

    printf("\nCPU         %s, %d cores / %d threads\n", cp->model, cp->cores, cp->threads);
    printf("  usage     %.1f%%  (user %.1f%%, system %.1f%%, iowait %.1f%%, steal %.1f%%)\n", cp->total, cp->user,
           cp->sys, cp->iowait, cp->steal);
    if (cp->mhz_avg > 0)
        printf("  clock     avg %.0f MHz, max %.0f MHz\n", cp->mhz_avg, cp->mhz_max);
    if (cp->temp_c >= 0)
        printf("  temp      %.1f C\n", cp->temp_c);
    printf("  load      %.2f %.2f %.2f\n", cp->load[0], cp->load[1], cp->load[2]);
    if (cp->governor[0] || cp->driver[0]) {
        printf("  power     %s", cp->driver[0] ? cp->driver : "?");
        if (cp->governor[0])
            printf(", governor %s", cp->governor);
        if (cp->epp[0])
            printf(", preference %s", cp->epp);
        if (cp->turbo >= 0)
            printf(", turbo %s", cp->turbo ? "on" : "off");
        if (cp->lim_max > 0)
            printf(", %.0f-%.0f MHz", cp->lim_min, cp->lim_max);
        if (cp->profile[0])
            printf(", profile %s", cp->profile);
        printf("\n");
    }
    const cpulim_t *L = &s->power.lim;
    if (L->ok && (L->eff_long_w > 0 || L->eff_short_w > 0)) {
        printf("  limits   ");
        if (L->eff_long_w > 0)
            printf(" %.0f W sustained", L->eff_long_w);
        if (L->eff_short_w > 0)
            printf("%s %.0f W in short boosts", L->eff_long_w > 0 ? "," : "", L->eff_short_w);
        printf("\n");
    }
    if (cp->nidle) {
        printf("  idle     ");
        for (int k = 0; k < cp->nidle; k++)
            if (strcmp(cp->idle[k].name, "POLL"))
                printf(" %s %.0f%%", cp->idle[k].name, cp->idle[k].pct);
        fmt_si(cp->wakeups, a, sizeof a);
        printf(", %s wake-ups/s\n", a);
    }
    printf("  per CPU  ");
    for (int i = 0; i < cp->ncpu; i++) {
        printf(" %d:%.0f%%", cp->id[i], cp->pct[i]);
        if (i % 12 == 11 && i + 1 < cp->ncpu)
            printf("\n           ");
    }
    printf("\n");

    fmt_bytes((double)m->used, a, sizeof a);
    fmt_bytes((double)m->total, b, sizeof b);
    fmt_bytes((double)m->avail, c, sizeof c);
    printf("\nMemory      %s / %s used (%.1f%%), %s available\n", a, b,
           m->total ? 100.0 * (double)m->used / (double)m->total : 0, c);
    fmt_bytes((double)m->swap_used, a, sizeof a);
    fmt_bytes((double)m->swap_total, b, sizeof b);
    printf("  swap      %s / %s\n", a, b);
    for (int i = 0; i < m->nswap; i++) {
        const swapdev_t *d = &m->swap[i];
        fmt_bytes((double)d->used, a, sizeof a);
        fmt_bytes((double)d->size, b, sizeof b);
        printf("    %-14s %-9s %s / %s", d->name, d->zram ? "zram" : d->file ? "file" : "partition", a, b);
        if (d->zram && d->compr && d->orig) {
            fmt_bytes((double)d->ram, c, sizeof c);
            printf(", %.1fx smaller (%s), takes %s of RAM", (double)d->orig / (double)d->compr, d->algo, c);
        }
        printf("\n");
    }

    report_health(s);

    printf("\nGPU\n");
    if (!s->gpus.n)
        printf("  none found\n");
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        printf("  %d %s [%s%s%s]\n", i, g->name, g->driver, g->pci[0] ? " " : "", g->pci);
        if (g->nv_state == NV_ASLEEP || g->nv_state == NV_IDLE) { /* not woken up for a snapshot */
            printf("    state   %s\n", g->note[0] ? g->note : g->nv_state == NV_ASLEEP ? "asleep (powered off)" : "idle");
            continue;
        }
        if (g->util >= 0) {
            printf("    load    %s%.1f%%%s", g->util_est ? "~" : "", g->util, g->util_est ? " (estimated from RC6 sleep time)" : "");
            for (int e = 0; e < g->neng; e++)
                printf("  %s %.0f%%", g->eng[e].name, g->eng[e].pct);
            printf("\n");
        } else if (g->note[0]) {
            printf("    load    %s\n", g->note);
        }
        if (g->mem_total > 0) {
            fmt_bytes(g->mem_used, a, sizeof a);
            fmt_bytes(g->mem_total, b, sizeof b);
            printf("    vram    %s / %s\n", a, b);
        }
        if (g->clk_mhz >= 0)
            printf("    clock   %.0f MHz\n", g->clk_mhz);
        if (g->temp_c >= 0 && g->temp_lim > 0)
            printf("    temp    %.1f C (slows down from %.0f C)\n", g->temp_c, g->temp_lim);
        else if (g->temp_c >= 0)
            printf("    temp    %.1f C\n", g->temp_c);
        if (g->slow_now)
            printf("    slowing %s\n", g->slow_now == (GS_HEAT | GS_POWER) ? "heat, power limit" : g->slow_now & GS_HEAT ? "heat" : "power limit");
        if (g->power_w >= 0)
            printf("    power   %.1f W\n", g->power_w);
    }

    power_t *p = &s->power;
    printf("\nPower\n");
    if (s->total_w >= 0)
        printf("  estimate  %.1f W (%s)\n", s->total_w, s->total_w_src);
    else if (s->total_why[0])
        printf("  estimate  the CPU part %s (sudo)\n", s->total_why);
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        if (z->readable && z->watts >= 0 && p->psys_bad && !strcmp(z->name, "psys"))
            printf("  %s%-10s%.2f W (less than the CPU: not connected, left out)\n", z->depth ? "  " : "", z->name, z->watts);
        else if (z->readable && z->watts >= 0)
            printf("  %s%-10s%.2f W\n", z->depth ? "  " : "", z->name, z->watts);
        else if (!z->readable)
            printf("  %s%-10sno permission (run as root)\n", z->depth ? "  " : "", z->name);
    }
    for (int i = 0; i < p->nbat; i++) {
        battery_t *bt = &p->bat[i];
        printf("  %-10s%s, %.0f%%", bt->name, bt->status, bt->capacity);
        if (bt->watts >= 0)
            printf(", %.2f W", bt->watts);
        if (bt->health >= 0)
            printf(", health %.0f%%", bt->health);
        if (bt->cycles >= 0)
            printf(", %ld cycles", bt->cycles);
        if (bt->lim_end > 0)
            printf(", charge limit %d%%", bt->lim_end);
        printf("\n");
    }
    if (!p->nrapl && !p->nbat)
        printf("  no power sensors found\n");

    printf("\nDisks\n");
    for (int i = 0; i < s->disks.n; i++) {
        disk_t *d = &s->disks.d[i];
        fmt_rate(d->rd_bps, a, sizeof a);
        fmt_rate(d->wr_bps, b, sizeof b);
        fmt_bytes((double)d->size, c, sizeof c);
        printf("  %-10s %-5s %6s  read %8s  write %8s  busy %3.0f%%  latency r %.1f w %.1f ms  %s\n", d->name, d->kind,
               c, a, b, d->util, d->rd_lat, d->wr_lat, d->model);
    }
    for (int i = 0; i < s->disks.nfs; i++) {
        fs_t *f = &s->disks.fs[i];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        printf("  fs %-20s %-6s %s / %s (%.0f%%)\n", f->mount, f->type, a, b,
               f->total ? 100.0 * (double)f->used / (double)f->total : 0);
    }

    printf("\nNetwork\n");
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        printf("  %-14s %-8s rx %8s  tx %8s  %s", f->name, f->state, a, b, f->addr);
        if (f->quality >= 0)
            printf("  wifi %.0f dBm (%d%%)", f->signal_dbm, f->quality);
        if (f->errors || f->drops)
            printf("  %llu errors, %llu dropped since boot", f->errors, f->drops);
        printf("\n");
    }

    if (s->usb.ok && s->usb.n) {
        printf("\nUSB\n");
        for (int i = 0; i < s->usb.n; i++) {
            const usbdev_t *d = &s->usb.d[i];
            char sp[24];
            usb_speed(d->mbps, sp, sizeof sp);
            printf("  %-8s %-30.30s %-14s %-11s %s%s%s\n", d->id, d->name, d->kind, sp,
                   d->where == 1 ? "socket" : d->where == 2 ? "built in" : d->where == 3 ? "via hub" : "",
                   d->blk[0] ? "  " : "", d->blk);
            if (d->slow)
                printf("  %-8s runs at USB 2 speed, can do %dG\n", "", d->can / 1000);
        }
    }

    if (s->sens.n) {
        printf("\nSensors\n");
        for (int i = 0; i < s->sens.n; i++) {
            sensor_t *t = &s->sens.s[i];
            if (!t->ok || t->hidden)
                continue;
            static const char *const unit[] = { "C", "RPM", "W", "V" };
            printf("  %-14s %-22s %8.*f %s%s\n", t->chip, sensor_label(t), t->type == SENS_VOLT ? 3 : 1, t->value,
                   unit[t->type], sensor_bad(t) ? "  (past your limit)" : "");
        }
    }

    if (nproc > 0 && s->procs.n) {
        proc_t **v = malloc((size_t)s->procs.n * sizeof *v);
        if (v) {
            for (int i = 0; i < s->procs.n; i++)
                v[i] = &s->procs.p[i];
            qsort(v, (size_t)s->procs.n, sizeof *v, by_cpu);
            printf("\nTop processes by CPU  (%d processes, %d threads)\n", s->procs.n, s->procs.threads);
            int gpu = s->procs.gpu_any;
            printf("  %7s %-10s %6s %6s %8s %9s %9s %6s%s  %s\n", "PID", "USER", "CPU%", "MEM%", "RSS", "READ/s",
                   "WRITE/s", "FILES", gpu ? "   GPU%" : "", "COMMAND");
            for (int i = 0; i < nproc && i < s->procs.n; i++) {
                proc_t *q = v[i];
                fmt_bytes((double)q->rss, a, sizeof a);
                if (q->io_ok) {
                    fmt_rate(q->rd_bps, b, sizeof b);
                    fmt_rate(q->wr_bps, c, sizeof c);
                } else {
                    str_copy(b, sizeof b, "-");
                    str_copy(c, sizeof c, "-");
                }
                const char *cmd = q->kthread ? "" : proc_cmdline(q);
                char fd[16] = "-", gp[16] = "";
                if (q->nfds >= 0)
                    sfmt(fd, sizeof fd, "%d", q->nfds);
                if (gpu && q->gpu >= 0)
                    sfmt(gp, sizeof gp, " %6.1f", q->gpu);
                else if (gpu)
                    sfmt(gp, sizeof gp, " %6s", "-");
                printf("  %7d %-10.10s %6.1f %6.1f %8s %9s %9s %6s%s  %.80s%s%s%s\n", (int)q->pid, uid_name(q->uid),
                       q->cpu, q->mem, a, b, c, fd, gp, cmd, *cmd ? "" : "[", *cmd ? "" : q->comm, *cmd ? "" : "]");
            }
            free(v);
        }
    }

    svcs_t *sv = &s->svc;
    if (sv->state == 1 && sv->n) {
        svc_t **v = malloc((size_t)sv->n * sizeof *v);
        if (v) {
            for (int i = 0; i < sv->n; i++)
                v[i] = &sv->s[i];
            qsort(v, (size_t)sv->n, sizeof *v, svc_by_mem);
            printf("\nServices and containers by memory  (%d)\n", sv->n);
            printf("  %-34s %-9s %6s %8s %9s %9s %6s\n", "NAME", "TYPE", "CPU%", "MEMORY", "READ/s", "WRITE/s", "TASKS");
            for (int i = 0; i < 12 && i < sv->n; i++) {
                svc_t *q = v[i];
                if (q->mem >= 0)
                    fmt_bytes((double)q->mem, a, sizeof a);
                else
                    str_copy(a, sizeof a, "-");
                fmt_rate(q->rd_bps, b, sizeof b);
                fmt_rate(q->wr_bps, c, sizeof c);
                printf("  %-34.34s %-9s %6.1f %8s %9s %9s %6ld%s\n", q->name, svc_kind_name(q->kind), q->cpu, a, b, c,
                       q->tasks, q->oom ? "  (OOM kills!)" : "");
            }
            free(v);
        }
    }
}

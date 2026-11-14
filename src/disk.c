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

/* disk.c - per-drive throughput (/proc/diskstats) and filesystem usage */
#define _GNU_SOURCE
#include "collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

/* Names already known not to be physical drives (partitions, loop, dm...). */
static char rejected[256][32];
static int nrejected;

static void sys_name(const char *name, char *out, size_t n)
{
    /* /sys/block uses '!' where the device name has '/' (e.g. cciss/c0d0) */
    str_copy(out, n, name);
    for (char *q = out; *q; q++)
        if (*q == '/')
            *q = '!';
}

static int is_physical(const char *name)
{
    char sn[32], p[PATHBUF];
    if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "zram", 4))
        return 0;
    sys_name(name, sn, sizeof sn);
    /* whole disks backed by real hardware have a "device" link;
     * partitions, dm-*, md*, loop and zram do not */
    sfmt(p, sizeof p, "%s/block/%s/device", g_sysfs, sn);
    return path_exists(p);
}

static void disk_static(disk_t *d)
{
    char sn[32], p[PATHBUF];
    long long v;
    sys_name(d->name, sn, sizeof sn);

    sfmt(p, sizeof p, "%s/block/%s/device/model", g_sysfs, sn);
    if (read_line(p, d->model, sizeof d->model))
        d->model[0] = '\0';
    if (!d->model[0] && !strncmp(d->name, "mmcblk", 6)) { /* an SD card or eMMC: its name, and which */
        char nm[32] = "", ty[16] = "";
        sfmt(p, sizeof p, "%s/block/%s/device/name", g_sysfs, sn);
        read_line(p, nm, sizeof nm);
        sfmt(p, sizeof p, "%s/block/%s/device/type", g_sysfs, sn);
        read_line(p, ty, sizeof ty);
        sfmt(d->model, sizeof d->model, "%s%s%s", ty[0] ? ty : "", ty[0] && nm[0] ? " " : "", nm);
    }
    sfmt(p, sizeof p, "%s/block/%s/size", g_sysfs, sn);
    if (!read_ll(p, &v))
        d->size = (unsigned long long)v * 512ULL;

    int rot = -1;
    sfmt(p, sizeof p, "%s/block/%s/queue/rotational", g_sysfs, sn);
    if (!read_ll(p, &v))
        rot = (int)v;
    if (!strncmp(d->name, "nvme", 4))
        str_copy(d->kind, sizeof d->kind, "NVMe");
    else if (!strncmp(d->name, "mmcblk", 6))
        str_copy(d->kind, sizeof d->kind, "MMC");
    else if (!strncmp(d->name, "sr", 2))
        str_copy(d->kind, sizeof d->kind, "ODD");
    else if (!strncmp(d->name, "vd", 2))
        str_copy(d->kind, sizeof d->kind, "virt");
    else if (rot == 1)
        str_copy(d->kind, sizeof d->kind, "HDD");
    else
        str_copy(d->kind, sizeof d->kind, "SSD");
}

static disk_t *find_disk(disks_t *ds, const char *name)
{
    for (int i = 0; i < ds->n; i++)
        if (!strcmp(ds->d[i].name, name))
            return &ds->d[i];
    return NULL;
}

static int is_rejected(const char *name)
{
    for (int i = 0; i < nrejected; i++)
        if (!strcmp(rejected[i], name))
            return 1;
    return 0;
}

static double dlt(unsigned long long a, unsigned long long b)
{
    return b >= a ? (double)(b - a) : 0;
}

void disks_sample(disks_t *ds, double dt)
{
    static char *buf;
    static size_t cap;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/diskstats", g_procfs);
    if (read_file_grow(path, &buf, &cap) <= 0)
        return;

    for (int i = 0; i < ds->n; i++)
        ds->d[i].seen = 0;

    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        unsigned maj, min;
        char name[32];
        unsigned long long rio, rmerge, rsec, rticks, wio, wmerge, wsec, wticks, inflight, ioticks;
        if (sscanf(line, " %u %u %31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &maj, &min, name, &rio,
                   &rmerge, &rsec, &rticks, &wio, &wmerge, &wsec, &wticks, &inflight, &ioticks) != 13)
            continue;
        disk_t *d = find_disk(ds, name);
        if (!d) {
            if (is_rejected(name))
                continue;
            if (!is_physical(name)) {
                if (nrejected < 256)
                    str_copy(rejected[nrejected++], sizeof rejected[0], name);
                continue;
            }
            if (ds->n >= MAX_DISKS)
                continue;
            d = &ds->d[ds->n++];
            memset(d, 0, sizeof *d);
            str_copy(d->name, sizeof d->name, name);
            disk_static(d);
        }
        d->seen = 1;
        if (d->primed && dt > 0) {
            d->rd_bps = dlt(d->p_rsec, rsec) * 512.0 / dt; /* diskstats sectors are always 512 B */
            d->wr_bps = dlt(d->p_wsec, wsec) * 512.0 / dt;
            d->rd_iops = dlt(d->p_rio, rio) / dt;
            d->wr_iops = dlt(d->p_wio, wio) / dt;
            d->util = CLAMP(dlt(d->p_ticks, ioticks) / (dt * 10.0), 0.0, 100.0); /* ms busy per s -> % */
            /* average time each request took, like iostat's r_await / w_await */
            double nr = dlt(d->p_rio, rio), nw = dlt(d->p_wio, wio);
            d->rd_lat = nr > 0 ? dlt(d->p_rms, rticks) / nr : 0;
            d->wr_lat = nw > 0 ? dlt(d->p_wms, wticks) / nw : 0;
        }
        d->inflight = (unsigned)inflight;
        d->p_rms = rticks;
        d->p_wms = wticks;
        d->p_rsec = rsec;
        d->p_wsec = wsec;
        d->p_rio = rio;
        d->p_wio = wio;
        d->p_ticks = ioticks;
        d->rd_total = rsec * 512ULL;
        d->wr_total = wsec * 512ULL;
        d->primed = 1;
    }

    /* drop drives that disappeared (USB stick unplugged) */
    int w = 0;
    for (int i = 0; i < ds->n; i++)
        if (ds->d[i].seen) {
            if (w != i)
                ds->d[w] = ds->d[i];
            w++;
        }
    ds->n = w;

    ds->rd_bps = ds->wr_bps = 0;
    for (int i = 0; i < ds->n; i++) {
        ds->rd_bps += ds->d[i].rd_bps;
        ds->wr_bps += ds->d[i].wr_bps;
    }
}

/* Only local, block-backed filesystems: statvfs() on a dead network mount
 * can block for minutes and would freeze the UI. */
static int fs_type_ok(const char *t)
{
    static const char *const ok[] = { "ext2",  "ext3",  "ext4",     "xfs",      "btrfs",   "f2fs",
                                      "vfat",  "exfat", "ntfs",     "ntfs3",    "fuseblk", "zfs",
                                      "jfs",   "reiserfs", "bcachefs", "hfsplus", "nilfs2", NULL };
    for (int i = 0; ok[i]; i++)
        if (!strcmp(t, ok[i]))
            return 1;
    return 0;
}

static void unescape_octal(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

void fs_sample(disks_t *ds)
{
    static char *buf;
    static size_t cap;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/self/mounts", g_procfs);
    if (read_file_grow(path, &buf, &cap) <= 0)
        return;

    ds->nfs = 0;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL && ds->nfs < MAX_FS) {
        char dev[96], mnt[256], type[16];
        if (sscanf(line, "%95s %255s %15s", dev, mnt, type) != 3 || !fs_type_ok(type))
            continue;
        int dup = 0;
        for (int i = 0; i < ds->nfs; i++)
            if (!strcmp(ds->fs[i].dev, dev))
                dup = 1; /* btrfs subvolumes, bind mounts */
        if (dup)
            continue;
        unescape_octal(mnt);
        struct statvfs sv;
        if (statvfs(mnt, &sv) != 0 || sv.f_blocks == 0)
            continue;
        fs_t *f = &ds->fs[ds->nfs++];
        str_copy(f->dev, sizeof f->dev, dev);
        str_copy(f->mount, sizeof f->mount, mnt);
        str_copy(f->type, sizeof f->type, type);
        f->total = (unsigned long long)sv.f_blocks * sv.f_frsize;
        f->avail = (unsigned long long)sv.f_bavail * sv.f_frsize;
        f->used = (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize;
        f->files = sv.f_files; /* 0 on btrfs, vfat...: no fixed number of files */
        f->ffree = sv.f_ffree <= sv.f_files ? sv.f_ffree : sv.f_files;
        f->readonly = (sv.f_flag & ST_RDONLY) != 0;
    }
}

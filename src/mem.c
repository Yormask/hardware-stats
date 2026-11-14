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

/* mem.c - RAM / swap usage (/proc/meminfo) and paging activity (/proc/vmstat) */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* "lzo lzo-rle lz4 [zstd]" -> "zstd" */
static void current_algo(const char *path, char *out, size_t n)
{
    char b[256];
    out[0] = '\0';
    if (read_line(path, b, sizeof b))
        return;
    char *l = strchr(b, '['), *r = l ? strchr(l, ']') : NULL;
    if (l && r) {
        *r = '\0';
        str_copy(out, n, l + 1);
    } else if (!strchr(b, ' ')) {
        str_copy(out, n, b);
    }
}

/* /proc/swaps writes a space in a name as \040 */
static void unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void zram_read(swapdev_t *d)
{
    char p[PATHBUF], b[256];
    sfmt(p, sizeof p, "%s/block/%s/mm_stat", g_sysfs, d->name);
    unsigned long long o = 0, c = 0, r = 0;
    if (read_line(p, b, sizeof b) == 0 && sscanf(b, "%llu %llu %llu", &o, &c, &r) == 3) {
        d->orig = o;
        d->compr = c;
        d->ram = r;
    }
    sfmt(p, sizeof p, "%s/block/%s/comp_algorithm", g_sysfs, d->name);
    current_algo(p, d->algo, sizeof d->algo);
}

/* the swap devices, and zram's compression */
static void swaps_read(mem_t *m)
{
    char path[PATHBUF], buf[8192];
    m->nswap = m->nzram = 0;
    sfmt(path, sizeof path, "%s/swaps", g_procfs);
    if (read_file(path, buf, sizeof buf) > 0) {
        char *cur = buf, *line;
        for (int first = 1; (line = next_line(&cur)) != NULL && m->nswap < MAX_SWAPS; first = 0) {
            char fn[256], type[32];
            unsigned long long size, used;
            int prio;
            if (first || sscanf(line, "%255s %31s %llu %llu %d", fn, type, &size, &used, &prio) != 5)
                continue;
            unescape(fn);
            swapdev_t *d = &m->swap[m->nswap++];
            memset(d, 0, sizeof *d);
            str_copy(d->name, sizeof d->name, !strncmp(fn, "/dev/", 5) ? fn + 5 : fn);
            d->file = !strcmp(type, "file");
            d->size = size * 1024ULL;
            d->used = used * 1024ULL;
            d->prio = prio;
            d->zram = !strncmp(d->name, "zram", 4) && isdigit((unsigned char)d->name[4]);
            if (d->zram) {
                zram_read(d);
                m->nzram++;
            }
        }
    }
    char b[16];
    sfmt(path, sizeof path, "%s/module/zswap/parameters/enabled", g_sysfs);
    m->zswap = read_line(path, b, sizeof b) ? -1 : (b[0] == 'Y' || b[0] == 'y' || b[0] == '1');
    m->zswap_stored = m->zswap_pool = 0;
    m->zswap_algo[0] = '\0';
    if (m->zswap == 1) {
        sfmt(path, sizeof path, "%s/module/zswap/parameters/compressor", g_sysfs);
        current_algo(path, m->zswap_algo, sizeof m->zswap_algo);
        long long v;
        long pg = sysconf(_SC_PAGESIZE);
        sfmt(path, sizeof path, "%s/kernel/debug/zswap/stored_pages", g_sysfs);
        if (read_ll(path, &v) == 0 && v > 0)
            m->zswap_stored = (unsigned long long)v * (unsigned long long)(pg > 0 ? pg : 4096);
        sfmt(path, sizeof path, "%s/kernel/debug/zswap/pool_total_size", g_sysfs);
        if (read_ll(path, &v) == 0 && v > 0)
            m->zswap_pool = (unsigned long long)v;
    }
}

void mem_sample(mem_t *m, double dt)
{
    static char buf[16384];
    static char *vbuf;
    static size_t vcap;
    char path[PATHBUF];

    sfmt(path, sizeof path, "%s/meminfo", g_procfs);
    if (read_file(path, buf, sizeof buf) > 0) {
        int have_avail = 0;
        char *cur = buf, *line;
        while ((line = next_line(&cur)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            unsigned long long v = strtoull(colon + 1, NULL, 10) * 1024ULL;
            if (!strcmp(line, "MemTotal"))
                m->total = v;
            else if (!strcmp(line, "MemFree"))
                m->free = v;
            else if (!strcmp(line, "MemAvailable")) {
                m->avail = v;
                have_avail = 1;
            } else if (!strcmp(line, "Buffers"))
                m->buffers = v;
            else if (!strcmp(line, "Cached"))
                m->cached = v;
            else if (!strcmp(line, "SReclaimable"))
                m->sreclaim = v;
            else if (!strcmp(line, "Shmem"))
                m->shmem = v;
            else if (!strcmp(line, "SwapTotal"))
                m->swap_total = v;
            else if (!strcmp(line, "SwapFree"))
                m->swap_free = v;
            else if (!strcmp(line, "Dirty"))
                m->dirty = v;
            else if (!strcmp(line, "Writeback"))
                m->writeback = v;
        }
        if (!have_avail) /* kernels older than 3.14 */
            m->avail = m->free + m->buffers + m->cached;
        m->used = m->total > m->avail ? m->total - m->avail : 0;
        m->buffcache = m->buffers + m->cached + m->sreclaim;
        m->swap_used = m->swap_total > m->swap_free ? m->swap_total - m->swap_free : 0;
    }

    sfmt(path, sizeof path, "%s/vmstat", g_procfs);
    if (read_file_grow(path, &vbuf, &vcap) > 0) {
        unsigned long long in = 0, out = 0, maj = 0;
        char *cur = vbuf, *line;
        while ((line = next_line(&cur)) != NULL) {
            if (!strncmp(line, "pswpin ", 7))
                in = strtoull(line + 7, NULL, 10);
            else if (!strncmp(line, "pswpout ", 8))
                out = strtoull(line + 8, NULL, 10);
            else if (!strncmp(line, "pgmajfault ", 11))
                maj = strtoull(line + 11, NULL, 10);
            else if (!strncmp(line, "oom_kill ", 9)) { /* Linux 4.13+ */
                m->oom_kill = strtoull(line + 9, NULL, 10);
                m->oom_ok = 1;
            }
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0)
            pg = 4096;
        if (m->primed && dt > 0) {
            m->swapin_bps = in >= m->p_swpin ? (double)(in - m->p_swpin) * pg / dt : 0;
            m->swapout_bps = out >= m->p_swpout ? (double)(out - m->p_swpout) * pg / dt : 0;
            m->majflt_rate = maj >= m->p_majflt ? (double)(maj - m->p_majflt) / dt : 0;
        }
        m->p_swpin = in;
        m->p_swpout = out;
        m->p_majflt = maj;
        m->primed = 1;
    }
    swaps_read(m);
}

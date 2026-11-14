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

/* usb.c - USB devices: what's plugged in, at what speed, from which port
 *
 * All from sysfs (/sys/bus/usb/devices), no root needed. A USB 3 drive
 * that runs at USB 2 speed is a common reason for slow copies: a USB 2
 * cable or hub, a plug not pushed in all the way, a USB 2 port. The
 * device's BOS descriptor (shown by kernel 6.10 and later) says what it
 * can do, the speed file what it runs at, and the port's "peer" link
 * whether the port itself can do USB 3.
 *
 * Connecting and disconnecting become events. A device that drops off and
 * comes back within 3 seconds is a warning: a loose plug or cable, or too
 * little power. Waking from sleep reconnects everything, so the first
 * seconds after a resume stay quiet. */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define GONE_WAIT 3.0     /* s: disconnected and not back by then, it's gone */
#define TELL_AFTER 1.0    /* s after connecting: its drivers have bound by then */
#define RESUME_QUIET 15.0 /* s after waking up: devices reconnect, that's no news */
#define BLK_EVERY 2.0     /* s between looks for the disk of a new drive */

enum { K_OTHER, K_FINGER, K_CARD, K_PRINTER, K_SERIAL, K_INPUT, K_MOUSE, K_KEYBOARD, K_BT,
       K_AUDIO, K_NET, K_WIFI, K_PHONE, K_CAMERA, K_STORAGE, K_HUB };
static const char *const KIND[] = { "other",    "fingerprint", "smart card", "printer", "serial",  "input",
                                    "mouse",    "keyboard",    "Bluetooth",  "audio",   "network", "Wi-Fi",
                                    "phone",    "camera",      "storage",    "hub" };

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* "1-2", "3-1.4": a device; not "usb1" (a root hub) nor "1-2:1.0" (one of its interfaces) */
static int is_dev(const char *n)
{
    if (!isdigit((unsigned char)n[0]) || strchr(n, ':'))
        return 0;
    const char *d = strchr(n, '-');
    return d && isdigit((unsigned char)d[1]);
}

/* by bus, then port numbers: 1-2 < 1-2.1 < 1-10 < 2-1 */
static int id_cmp(const void *a, const void *b)
{
    const char *p = ((const usbdev_t *)a)->id, *q = ((const usbdev_t *)b)->id;
    while (*p && *q) {
        if (isdigit((unsigned char)*p) && isdigit((unsigned char)*q)) {
            char *e, *f;
            long x = strtol(p, &e, 10), y = strtol(q, &f, 10);
            if (x != y)
                return x < y ? -1 : 1;
            p = e;
            q = f;
        } else {
            if (*p != *q)
                return (unsigned char)*p - (unsigned char)*q;
            p++;
            q++;
        }
    }
    return (unsigned char)*p - (unsigned char)*q;
}

static unsigned long long mix(const char *s, long long n)
{
    unsigned long long h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    h ^= (unsigned long long)n;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/* trimmed, spaces squeezed; control characters out (strings come from the device) */
static void tidy(char *s)
{
    char *w = s;
    int sp = 1;
    for (const char *r = s; *r; r++) {
        unsigned char ch = (unsigned char)*r;
        if (ch < 32 || ch == 127)
            ch = ' ';
        if (ch == ' ') {
            if (sp)
                continue;
            sp = 1;
        } else {
            sp = 0;
        }
        *w++ = (char)ch;
    }
    while (w > s && w[-1] == ' ')
        w--;
    *w = '\0';
}

/* "Chicony Electronics Co.,Ltd." -> "Chicony", "Intel(R) Corporation" -> "Intel" */
static void short_maker(char *m)
{
    static const char *const tail[] = { "Co.,Ltd.", "Co., Ltd.", "Co.,Ltd", "Co., Ltd", "Co. Ltd.", "Co.Ltd.",
                                        "Ltd.", "Ltd", "Inc.", "Inc", "Corp.", "Corp", "Corporation", "Limited",
                                        "Electronics", "Electronic", "Technology", "Technologies", "Semiconductor",
                                        "International", "GmbH", "AG", "S.A.", "LLC", "Co.", "(R)", "(TM)", "," };
    char *r;
    while ((r = strstr(m, "(R)")) != NULL || (r = strstr(m, "(TM)")) != NULL)
        memmove(r, r + (r[1] == 'R' ? 3 : 4), strlen(r + (r[1] == 'R' ? 3 : 4)) + 1);
    for (int again = 1; again;) {
        again = 0;
        tidy(m);
        size_t n = strlen(m);
        for (size_t i = 0; i < sizeof tail / sizeof tail[0]; i++) {
            size_t t = strlen(tail[i]);
            if (n > t && !strcasecmp(m + n - t, tail[i]) && (m[n - t - 1] == ' ' || m[n - t - 1] == ',' || tail[i][0] == ',')) {
                m[n - t] = '\0';
                again = 1;
                break;
            }
        }
    }
    tidy(m);
    size_t n = strlen(m);
    while (n && m[n - 1] == ',')
        m[--n] = '\0';
}

/* usb.ids, for devices that don't name themselves */
static int ids_lookup(unsigned vid, unsigned pid, char *vend, size_t vn, char *dev, size_t dn)
{
    const char *files[] = { secure_getenv("HSM_USB_IDS"), "/usr/share/hwdata/usb.ids", "/usr/share/misc/usb.ids",
                            "/usr/share/usb.ids", "/var/lib/usbutils/usb.ids" };
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !f; i++)
        if (files[i])
            f = fopen(files[i], "re");
    if (!f)
        return -1;
    char line[512];
    int in = 0;
    vend[0] = dev[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || !line[0])
            continue;
        if (line[0] != '\t') {
            if (in)
                break;
            unsigned v;
            if (isxdigit((unsigned char)line[0]) && isxdigit((unsigned char)line[3]) && line[4] == ' ' &&
                sscanf(line, "%4x", &v) == 1 && v == vid) {
                in = 1;
                str_copy(vend, vn, line + 4);
                tidy(vend);
            }
            continue;
        }
        unsigned d;
        if (in && line[1] != '\t' && sscanf(line + 1, "%4x", &d) == 1 && d == pid) {
            str_copy(dev, dn, line + 5);
            tidy(dev);
            break;
        }
    }
    fclose(f);
    return vend[0] ? 0 : -1;
}

/* the most it can do, from its BOS descriptor: 5000 or more for USB 3, 480 for USB 2, 0 unknown */
static int bos_speed(const char *dir)
{
    char p[PATHBUF], b[1024];
    sfmt(p, sizeof p, "%s/bos_descriptors", dir);
    ssize_t n = read_file(p, b, sizeof b);
    if (n < 5)
        return 0;
    const unsigned char *u = (const unsigned char *)b;
    if (u[1] != 0x0f) /* not a BOS */
        return 0;
    int can = 480, off = u[0];
    while (off + 3 <= n) {
        int len = u[off];
        if (len < 3 || off + len > n)
            break;
        if (u[off + 1] == 0x10) { /* a device capability */
            int type = u[off + 2];
            if (type == 0x03 && len >= 10 && (u[off + 4] & 0x08)) { /* SuperSpeed: 5 Gb/s */
                can = can > 5000 ? can : 5000;
            } else if (type == 0x0a && len >= 12) { /* SuperSpeedPlus: the sublink speeds */
                int nattr = (u[off + 4] & 0x1f) + 1;
                can = can > 10000 ? can : 10000;
                for (int k = 0; k < nattr && off + 12 + 4 * k + 4 <= off + len; k++) {
                    const unsigned char *a = u + off + 12 + 4 * k;
                    unsigned attr = (unsigned)a[0] | (unsigned)a[1] << 8 | (unsigned)a[2] << 16 | (unsigned)a[3] << 24;
                    unsigned lse = (attr >> 4) & 3, lsm = attr >> 16, lp = (attr >> 14) & 3;
                    if (lp == 1 && lse == 3 && lsm >= 10 && lsm <= 40) /* Gb/s */
                        can = can > (int)lsm * 1000 ? can : (int)lsm * 1000;
                }
            }
        }
        off += len;
    }
    return can;
}

static int driver_kind(const char *drv)
{
    static const struct {
        const char *name;
        int kind;
    } D[] = { { "usb-storage", K_STORAGE }, { "uas", K_STORAGE },    { "uvcvideo", K_CAMERA },
              { "snd-usb-audio", K_AUDIO }, { "btusb", K_BT },       { "r8152", K_NET },
              { "cdc_ether", K_NET },       { "cdc_ncm", K_NET },    { "ax88179_178a", K_NET },
              { "asix", K_NET },            { "rndis_host", K_NET }, { "ipheth", K_NET },
              { "cdc_mbim", K_NET },        { "qmi_wwan", K_NET },   { "cdc_acm", K_SERIAL },
              { "ch341", K_SERIAL },        { "cp210x", K_SERIAL },  { "ftdi_sio", K_SERIAL },
              { "pl2303", K_SERIAL },       { "usblp", K_PRINTER },  { "hub", K_HUB } };
    static const char *const wifi[] = { "rtw88", "rtw89", "rtl8xxxu", "rtl8187", "rtl8192", "rtl88", "mt7601u",
                                        "mt76", "mt79", "ath9k_htc", "ath10k_usb", "ath11k", "carl9170",
                                        "rt2800usb", "rt73usb", "brcmfmac", "8821", "8812", "88x2", "rtl8814" };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++)
        if (!strcmp(drv, D[i].name))
            return D[i].kind;
    for (size_t i = 0; i < sizeof wifi / sizeof wifi[0]; i++)
        if (!strncmp(drv, wifi[i], strlen(wifi[i])))
            return K_WIFI;
    return -1;
}

static int class_kind(unsigned cls, unsigned sub, unsigned proto, const char *ifname, unsigned vid)
{
    switch (cls) {
    case 0x01: return K_AUDIO;
    case 0x02: return sub == 0x02 ? K_SERIAL : K_NET;
    case 0x03: return proto == 1 ? K_KEYBOARD : proto == 2 ? K_MOUSE : K_INPUT;
    case 0x06: return strstr(ifname, "MTP") ? K_PHONE : K_CAMERA;
    case 0x07: return K_PRINTER;
    case 0x08: return K_STORAGE;
    case 0x09: return K_HUB;
    case 0x0b: return K_CARD;
    case 0x0e: return K_CAMERA;
    case 0xe0: return sub == 0x01 && proto == 0x01 ? K_BT : sub == 0x01 && proto == 0x03 ? K_NET : K_OTHER;
    case 0xef: return sub == 0x04 && proto == 0x01 ? K_NET : K_OTHER;
    case 0xff:
        if (strstr(ifname, "MTP"))
            return K_PHONE;
        /* fingerprint readers in laptops: Goodix, Synaptics, Validity, Elan, FocalTech, LighTuning, FPC */
        if (vid == 0x27c6 || vid == 0x06cb || vid == 0x138a || vid == 0x04f3 || vid == 0x2808 || vid == 0x1c7a ||
            vid == 0x10a5)
            return K_FINGER;
        return K_OTHER;
    }
    return K_OTHER;
}

/* what it is, from its interfaces: the most telling one wins (a webcam's video over its microphone) */
static void read_kind(const char *dir, const char *id, unsigned devcls, usbdev_t *d)
{
    int best = devcls == 0x09 ? K_HUB : -1, mouse = 0;
    DIR *dd = opendir(dir);
    if (dd) {
        struct dirent *e;
        size_t il = strlen(id);
        while ((e = readdir(dd)) != NULL) {
            if (strncmp(e->d_name, id, il) || e->d_name[il] != ':')
                continue;
            char p[PATHBUF], ifname[64] = "", lnk[PATHBUF];
            unsigned cls = 0, sub = 0, proto = 0;
            sfmt(p, sizeof p, "%s/%s/bInterfaceClass", dir, e->d_name);
            if (read_hex(p, &cls))
                continue;
            sfmt(p, sizeof p, "%s/%s/bInterfaceSubClass", dir, e->d_name);
            read_hex(p, &sub);
            sfmt(p, sizeof p, "%s/%s/bInterfaceProtocol", dir, e->d_name);
            read_hex(p, &proto);
            sfmt(p, sizeof p, "%s/%s/interface", dir, e->d_name);
            if (read_line(p, ifname, sizeof ifname))
                ifname[0] = '\0';
            int k = -1;
            sfmt(p, sizeof p, "%s/%s/driver", dir, e->d_name);
            ssize_t n = readlink(p, lnk, sizeof lnk - 1);
            if (n > 0) {
                lnk[n] = '\0';
                const char *b = strrchr(lnk, '/');
                k = driver_kind(b ? b + 1 : lnk);
            }
            if (k < 0)
                k = class_kind(cls, sub, proto, ifname, d->vid);
            if (cls == 0x03 && proto == 2)
                mouse = 1;
            if (k > best)
                best = k;
        }
        closedir(dd);
    }
    if (best < 0)
        best = K_OTHER;
    if (best == K_KEYBOARD && mouse) /* a wireless receiver for both */
        str_copy(d->kind, sizeof d->kind, "keyboard/mouse");
    else
        str_copy(d->kind, sizeof d->kind, KIND[best]);
}

static void read_name(const char *dir, usbdev_t *d)
{
    char p[PATHBUF], prod[64] = "", maker[64] = "";
    sfmt(p, sizeof p, "%s/product", dir);
    if (read_line(p, prod, sizeof prod))
        prod[0] = '\0';
    sfmt(p, sizeof p, "%s/manufacturer", dir);
    if (read_line(p, maker, sizeof maker))
        maker[0] = '\0';
    tidy(prod);
    short_maker(maker);
    if (!strcasecmp(maker, "USB") || !strcasecmp(maker, "Manufacturer"))
        maker[0] = '\0';
    if (!prod[0]) { /* nameless: usb.ids, if it's there */
        char v[64], n[64];
        if (ids_lookup(d->vid, d->pid, v, sizeof v, n, sizeof n) == 0) {
            short_maker(v);
            if (n[0])
                str_copy(prod, sizeof prod, n);
            if (!maker[0])
                str_copy(maker, sizeof maker, v);
        }
    }
    if (prod[0] && maker[0] && strlen(maker) <= 16 && !strcasestr(prod, maker))
        sfmt(d->name, sizeof d->name, "%s %s", maker, prod);
    else if (prod[0])
        str_copy(d->name, sizeof d->name, prod);
    else if (maker[0])
        str_copy(d->name, sizeof d->name, maker);
    else
        sfmt(d->name, sizeof d->name, "%04x:%04x", d->vid, d->pid);
}

/* the port it's in: an outside socket or built in, and whether it can do USB 3 */
static void read_port(const char *base, const char *dir, usbdev_t *d)
{
    char p[PATHBUF], t[32];
    sfmt(p, sizeof p, "%s/port", dir);
    if (access(p, F_OK) != 0) { /* older kernels: the hub's port by name */
        const char *dot = strrchr(d->id, '.'), *dash = strchr(d->id, '-');
        if (!dash)
            return;
        int bus = atoi(d->id), port = atoi(dot ? dot + 1 : dash + 1);
        if (dot) {
            char parent[20];
            sfmt(parent, sizeof parent, "%.*s", (int)(dot - d->id), d->id);
            sfmt(p, sizeof p, "%s/%s/%s:1.0/%s-port%d", base, parent, parent, parent, port);
        } else {
            sfmt(p, sizeof p, "%s/usb%d/%d-0:1.0/usb%d-port%d", base, bus, bus, bus, port);
        }
        if (access(p, F_OK) != 0)
            return;
    }
    char q[PATHBUF + 16];
    sfmt(q, sizeof q, "%s/peer", p);
    d->port3 = access(q, F_OK) == 0;
    if (d->depth) {
        d->where = 3;
        return;
    }
    sfmt(q, sizeof q, "%s/connect_type", p);
    if (read_line(q, t, sizeof t) == 0)
        d->where = !strcmp(t, "hotplug") ? 1 : !strcmp(t, "hardwired") ? 2 : 0;
}

static int read_dev(const char *base, const char *id, int devnum, usbdev_t *d, const usb_t *old)
{
    char dir[PATHBUF], p[PATHBUF + 24], b[32];
    sfmt(dir, sizeof dir, "%s/%s", base, id);
    memset(d, 0, sizeof *d);
    str_copy(d->id, sizeof d->id, id);
    d->devnum = devnum;
    d->port3 = -1;
    sfmt(p, sizeof p, "%s/idVendor", dir);
    if (read_hex(p, &d->vid))
        return -1;
    sfmt(p, sizeof p, "%s/idProduct", dir);
    read_hex(p, &d->pid);
    sfmt(p, sizeof p, "%s/speed", dir);
    if (read_line(p, b, sizeof b) == 0)
        d->mbps = strtod(b, NULL);
    sfmt(p, sizeof p, "%s/bMaxPower", dir);
    if (read_line(p, b, sizeof b) == 0)
        d->ma = atoi(b);
    unsigned cls = 0;
    sfmt(p, sizeof p, "%s/bDeviceClass", dir);
    read_hex(p, &cls);
    for (const char *c = id; *c; c++)
        d->depth += *c == '.';
    const usbdev_t *o = NULL; /* the same connection as before: its name is known already */
    for (int i = 0; old && i < old->n; i++)
        if (!strcmp(old->d[i].id, id) && old->d[i].devnum == devnum && old->d[i].vid == d->vid &&
            old->d[i].pid == d->pid)
            o = &old->d[i];
    if (o && o->name[0])
        str_copy(d->name, sizeof d->name, o->name);
    else
        read_name(dir, d);
    read_kind(dir, id, cls, d);
    d->can = bos_speed(dir);
    read_port(base, dir, d);
    d->slow = d->can >= 5000 && d->mbps > 0 && d->mbps <= 480 && strcmp(d->kind, "hub");
    if (o)
        str_copy(d->blk, sizeof d->blk, o->blk);
    return 0;
}

/* the disks USB drives give: /sys/block/sdb leads through the USB device "2-1" */
static void map_disks(usb_t *u)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/block", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
            !strncmp(e->d_name, "zram", 4) || !strncmp(e->d_name, "dm-", 3) || !strncmp(e->d_name, "md", 2) ||
            !strncmp(e->d_name, "nvme", 4))
            continue;
        char p[PATHBUF], real[PATH_MAX];
        sfmt(p, sizeof p, "%s/%s", base, e->d_name);
        if (!realpath(p, real) || !strstr(real, "/usb"))
            continue;
        char last[20] = "", *save = NULL; /* the last "B-P.P" on the way: the device it belongs to */
        for (char *t = strtok_r(real, "/", &save); t; t = strtok_r(NULL, "/", &save))
            if (is_dev(t) && strlen(t) < sizeof last)
                str_copy(last, sizeof last, t);
        for (int i = 0; last[0] && i < u->n; i++)
            if (!strcmp(u->d[i].id, last) && (!u->d[i].blk[0] || strcmp(e->d_name, u->d[i].blk) < 0))
                str_copy(u->d[i].blk, sizeof u->d[i].blk, e->d_name);
    }
    closedir(d);
}

const char *usb_speed(double mbps, char *out, size_t n)
{
    if (mbps <= 0)
        str_copy(out, n, "?");
    else if (mbps < 12)
        sfmt(out, n, "USB 1 %gM", mbps);
    else if (mbps < 480)
        sfmt(out, n, "USB 1 %.0fM", mbps);
    else if (mbps < 5000)
        sfmt(out, n, "USB 2 %.0fM", mbps);
    else
        sfmt(out, n, "USB 3 %.0fG", mbps / 1000);
    return out;
}

static int matters(const usbdev_t *d) /* slow at USB 2 speed: copies and downloads */
{
    return !strcmp(d->kind, "storage") || !strcmp(d->kind, "network") || !strcmp(d->kind, "Wi-Fi");
}

static void tell_connect(health_t *h, const usbdev_t *d)
{
    char sp[24], can[24];
    usb_speed(d->mbps, sp, sizeof sp);
    if (d->slow) {
        sfmt(can, sizeof can, "%dG", d->can / 1000);
        health_event(h, matters(d) ? EV_WARN : EV_INFO, "usb", "%s connected at USB 2 speed, though it can do %s (%s, port %s)",
                     d->name, can, d->kind, d->id);
    } else {
        health_event(h, EV_INFO, "usb", "%s connected: %s, %s, port %s", d->name, d->kind, sp, d->id);
    }
}

void usb_sample(sys_t *s)
{
    usb_t *u = &s->usb;
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/bus/usb/devices", g_sysfs);
    DIR *dir = opendir(base);
    if (!dir) {
        u->ok = 0;
        return;
    }
    struct {
        char id[20];
        int devnum;
    } cur[MAX_USB];
    int nc = 0;
    unsigned long long sig = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && nc < MAX_USB) {
        if (!is_dev(e->d_name) || strlen(e->d_name) >= sizeof cur[0].id)
            continue;
        char p[PATHBUF];
        long long dn;
        sfmt(p, sizeof p, "%s/%s/devnum", base, e->d_name);
        if (read_ll(p, &dn)) /* going away right now */
            continue;
        str_copy(cur[nc].id, sizeof cur[nc].id, e->d_name);
        cur[nc].devnum = (int)dn;
        sig ^= mix(e->d_name, dn);
        nc++;
    }
    closedir(dir);
    u->ok = 1;
    double now = wall();
    int quiet = !u->primed || (s->health.resume_t > 0 && now - s->health.resume_t < RESUME_QUIET);
    int pending = u->ngone > 0;
    for (int i = 0; i < u->n; i++)
        pending |= !u->d[i].told;
    if (sig != u->sig || !u->primed || pending) {
        static usbdev_t nw[MAX_USB];
        int nn = 0;
        for (int i = 0; i < nc; i++)
            if (read_dev(base, cur[i].id, cur[i].devnum, &nw[nn], u) == 0)
                nn++;
        qsort(nw, (size_t)nn, sizeof nw[0], id_cmp);
        for (int k = 0; k < u->n; k++) { /* gone: told in 3 s, unless it comes back (a new device number: at once) */
            const usbdev_t *o = &u->d[k];
            int there = 0;
            for (int i = 0; i < nn && !there; i++)
                there = !strcmp(nw[i].id, o->id) && nw[i].devnum == o->devnum;
            if (there || !o->told || quiet)
                continue;
            if (u->ngone == (int)(sizeof u->gone / sizeof u->gone[0])) { /* full: the oldest is told now */
                health_event(&s->health, EV_INFO, "usb", "%s disconnected (port %s)", u->gone[0].d.name, u->gone[0].d.id);
                memmove(&u->gone[0], &u->gone[1], sizeof u->gone[0] * (size_t)(u->ngone - 1));
                u->ngone--;
            }
            u->gone[u->ngone].d = *o;
            u->gone[u->ngone].t = now;
            u->ngone++;
        }
        for (int i = 0; i < nn; i++) { /* the same as before, new, or back after dropping off */
            usbdev_t *d = &nw[i];
            const usbdev_t *o = NULL;
            for (int k = 0; k < u->n && !o; k++)
                if (!strcmp(u->d[k].id, d->id) && u->d[k].devnum == d->devnum)
                    o = &u->d[k];
            if (o) {
                d->seen = o->seen;
                d->told = o->told;
                continue;
            }
            d->seen = u->primed ? now : 0;
            d->told = quiet;
            for (int g = 0; g < u->ngone; g++) {
                const usbdev_t *w = &u->gone[g].d;
                if (strcmp(w->id, d->id) || w->vid != d->vid || w->pid != d->pid)
                    continue;
                if (!quiet)
                    health_event(&s->health, EV_WARN, "usb", "%s dropped off and came back at once (port %s)%s", d->name,
                                 d->id, d->slow && !w->slow ? ", now at USB 2 speed" : "");
                d->told = 1;
                u->gone[g] = u->gone[--u->ngone];
                break;
            }
        }
        memcpy(u->d, nw, sizeof nw[0] * (size_t)nn);
        u->n = nn;
        u->sig = sig;
        if (!u->primed)
            u->next_blk = 0;
        u->primed = 1;
    }
    for (int g = 0; g < u->ngone;) { /* not back in time: gone */
        if (quiet) {
            u->gone[g] = u->gone[--u->ngone];
        } else if (now - u->gone[g].t >= GONE_WAIT) {
            health_event(&s->health, EV_INFO, "usb", "%s disconnected (port %s)", u->gone[g].d.name, u->gone[g].d.id);
            u->gone[g] = u->gone[--u->ngone];
        } else {
            g++;
        }
    }
    int noblk = 0;
    for (int i = 0; i < u->n; i++)
        noblk |= !strcmp(u->d[i].kind, "storage") && !u->d[i].blk[0];
    if (noblk && now >= u->next_blk) {
        u->next_blk = now + BLK_EVERY;
        map_disks(u);
    }
    u->nslow = 0;
    for (int i = 0; i < u->n; i++) {
        usbdev_t *d = &u->d[i];
        u->nslow += d->slow && matters(d);
        if (!d->told && now - d->seen >= TELL_AFTER) {
            d->told = 1;
            if (!quiet)
                tell_connect(&s->health, d);
        }
    }
}

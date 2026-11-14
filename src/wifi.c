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

/* wifi.c - Wi-Fi details: band, channel, width, link speed, Wi-Fi generation
 *
 * From the kernel's nl80211 interface over generic netlink, as `iw dev wlan0
 * link` shows it; no root needed. One socket, asked every sample for each
 * wireless interface that is up. A kernel without Wi-Fi support (no nl80211
 * family) is asked again once a minute. The signal comes from here too when
 * /proc/net/wireless is missing (kernels without the old wireless
 * extensions). HSM_WIFI_FAKE=dir answers from files instead (dir/wlan0.iface
 * and dir/wlan0.station, the bytes the kernel sends), to test without Wi-Fi. */
#define _GNU_SOURCE
#include "collect.h"

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* nl80211 values: a stable kernel interface, so its header isn't needed to build */
enum { WF_CMD_GET_INTERFACE = 5, WF_CMD_GET_STATION = 17 };
enum { WF_ATTR_IFINDEX = 3, WF_ATTR_STA_INFO = 21, WF_ATTR_WIPHY_FREQ = 38, WF_ATTR_CHANNEL_WIDTH = 159,
       WF_ATTR_CENTER_FREQ1 = 160, WF_ATTR_MAX = 161 };
enum { WF_STA_SIGNAL = 7, WF_STA_TX_BITRATE = 8, WF_STA_TX_PACKETS = 10, WF_STA_TX_RETRIES = 11,
       WF_STA_TX_FAILED = 12, WF_STA_RX_BITRATE = 14, WF_STA_CONNECTED_TIME = 16, WF_STA_BEACON_LOSS = 18,
       WF_STA_MAX = 19 };
enum { WF_RATE_BITRATE = 1, WF_RATE_MCS = 2, WF_RATE_BITRATE32 = 5, WF_RATE_VHT_MCS = 6, WF_RATE_HE_MCS = 13,
       WF_RATE_EHT_MCS = 19, WF_RATE_MAX = 21 };
enum { WF_W20_NOHT, WF_W20, WF_W40, WF_W80, WF_W80P80, WF_W160, WF_W5, WF_W10, WF_W1, WF_W2, WF_W4, WF_W8,
       WF_W16, WF_W320 };
#if defined(__has_include)
#if __has_include(<linux/nl80211.h>)
#include <linux/nl80211.h>
#define SAME(a, b) ((int)(a) == (int)(b))
_Static_assert(SAME(NL80211_CMD_GET_INTERFACE, WF_CMD_GET_INTERFACE) && SAME(NL80211_CMD_GET_STATION, WF_CMD_GET_STATION) &&
                   SAME(NL80211_ATTR_STA_INFO, WF_ATTR_STA_INFO) && SAME(NL80211_ATTR_WIPHY_FREQ, WF_ATTR_WIPHY_FREQ) &&
                   SAME(NL80211_ATTR_CHANNEL_WIDTH, WF_ATTR_CHANNEL_WIDTH) &&
                   SAME(NL80211_ATTR_CENTER_FREQ1, WF_ATTR_CENTER_FREQ1) && SAME(NL80211_STA_INFO_SIGNAL, WF_STA_SIGNAL) &&
                   SAME(NL80211_STA_INFO_TX_PACKETS, WF_STA_TX_PACKETS) && SAME(NL80211_STA_INFO_TX_RETRIES, WF_STA_TX_RETRIES) &&
                   SAME(NL80211_STA_INFO_CONNECTED_TIME, WF_STA_CONNECTED_TIME) &&
                   SAME(NL80211_STA_INFO_BEACON_LOSS, WF_STA_BEACON_LOSS) && SAME(NL80211_STA_INFO_RX_BITRATE, WF_STA_RX_BITRATE) &&
                   SAME(NL80211_RATE_INFO_BITRATE32, WF_RATE_BITRATE32) && SAME(NL80211_RATE_INFO_VHT_MCS, WF_RATE_VHT_MCS) &&
                   SAME(NL80211_RATE_INFO_HE_MCS, WF_RATE_HE_MCS) && SAME(NL80211_RATE_INFO_EHT_MCS, WF_RATE_EHT_MCS) &&
                   SAME(NL80211_CHAN_WIDTH_160, WF_W160) && SAME(NL80211_CHAN_WIDTH_320, WF_W320),
               "nl80211 values");
#undef SAME
#endif
#endif

#define RETRY_EVERY 10.0 /* s: the share of frames sent again, over this long */
#define BAND_SETTLE 10.0 /* s on another band before it's an event (not a blip while roaming) */

static int g_fd = -1;
static int g_fam = -1; /* nl80211's family number; -1 not asked yet, 0 none */
static double g_retry_at;
static unsigned g_seq;

typedef struct {
    const unsigned char *p;
    int len;
} span_t;

static void attrs(const unsigned char *p, int len, span_t *out, int max)
{
    memset(out, 0, sizeof *out * (size_t)max);
    while (len >= NLA_HDRLEN) {
        struct nlattr a;
        memcpy(&a, p, sizeof a);
        if (a.nla_len < NLA_HDRLEN || a.nla_len > len)
            break;
        int t = a.nla_type & NLA_TYPE_MASK;
        if (t < max) {
            out[t].p = p + NLA_HDRLEN;
            out[t].len = a.nla_len - NLA_HDRLEN;
        }
        int step = NLA_ALIGN(a.nla_len);
        if (step >= len)
            break;
        p += step;
        len -= step;
    }
}

static unsigned u32(span_t s)
{
    uint32_t v = 0;
    if (s.len >= 4)
        memcpy(&v, s.p, 4);
    else if (s.len == 2) {
        uint16_t w;
        memcpy(&w, s.p, 2);
        v = w;
    } else if (s.len == 1)
        v = s.p[0];
    return v;
}

static const unsigned char *payload(const struct nlmsghdr *h, int *len)
{
    *len = (int)h->nlmsg_len - (int)NLMSG_LENGTH(GENL_HDRLEN);
    return (const unsigned char *)h + NLMSG_LENGTH(GENL_HDRLEN);
}

typedef void (*msg_fn)(const struct nlmsghdr *h, void *ctx);

/* the messages in buf: each answer to seq (0: any) to fn. 1 done (or an
 * error: *err), 0 more to come (a dump), -1 nonsense */
static int walk(const char *buf, size_t n, unsigned seq, int dump, msg_fn fn, void *ctx, int *err)
{
    size_t off = 0;
    while (off + sizeof(struct nlmsghdr) <= n) {
        struct nlmsghdr h;
        memcpy(&h, buf + off, sizeof h);
        if (h.nlmsg_len < sizeof h || off + h.nlmsg_len > n)
            return -1;
        const struct nlmsghdr *m = (const struct nlmsghdr *)(const void *)(buf + off);
        off += NLMSG_ALIGN(h.nlmsg_len);
        if (seq && h.nlmsg_seq != seq) /* a late answer to an older question */
            continue;
        if (h.nlmsg_type == NLMSG_DONE)
            return 1;
        if (h.nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr e;
            if (h.nlmsg_len >= NLMSG_LENGTH(sizeof e)) {
                memcpy(&e, NLMSG_DATA(m), sizeof e);
                *err = -e.error;
            }
            return 1;
        }
        if (h.nlmsg_len >= NLMSG_LENGTH(GENL_HDRLEN))
            fn(m, ctx);
        if (!dump)
            return 1;
    }
    return 0;
}

static int nl_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0)
        return -1;
    struct timeval tv = { 0, 250000 }; /* never hold the sample up for long */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_nl a;
    memset(&a, 0, sizeof a);
    a.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* one question: family, command, flags and one attribute; the answers to fn. 0 ok, else an errno */
static int ask(int fam, int cmd, int flags, int atype, const void *adata, int alen, msg_fn fn, void *ctx)
{
    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        unsigned char a[64];
    } r;
    memset(&r, 0, sizeof r);
    if (alen < 0 || NLA_HDRLEN + alen > (int)sizeof r.a)
        return EINVAL;
    r.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    r.n.nlmsg_type = (uint16_t)fam;
    r.n.nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags);
    r.n.nlmsg_seq = ++g_seq;
    r.g.cmd = (uint8_t)cmd;
    r.g.version = 1;
    struct nlattr na = { (uint16_t)(NLA_HDRLEN + alen), (uint16_t)atype };
    memcpy(r.a, &na, sizeof na);
    memcpy(r.a + NLA_HDRLEN, adata, (size_t)alen);
    r.n.nlmsg_len += (uint32_t)NLA_ALIGN(NLA_HDRLEN + alen);
    struct sockaddr_nl to;
    memset(&to, 0, sizeof to);
    to.nl_family = AF_NETLINK;
    if (sendto(g_fd, &r, r.n.nlmsg_len, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)r.n.nlmsg_len)
        return errno ? errno : EIO;
    static char buf[32768] __attribute__((aligned(8)));
    for (int round = 0; round < 64; round++) {
        ssize_t n = recv(g_fd, buf, sizeof buf, 0);
        if (n < 0)
            return errno == EINTR ? EINTR : errno ? errno : EIO;
        int err = 0, w = walk(buf, (size_t)n, r.n.nlmsg_seq, flags & NLM_F_DUMP, fn, ctx, &err);
        if (w)
            return w < 0 ? EIO : err;
    }
    return EIO;
}

static void family_cb(const struct nlmsghdr *h, void *ctx)
{
    int len;
    const unsigned char *p = payload(h, &len);
    span_t a[CTRL_ATTR_FAMILY_NAME + 1];
    attrs(p, len, a, CTRL_ATTR_FAMILY_NAME + 1);
    if (a[CTRL_ATTR_FAMILY_ID].len >= 2)
        *(int *)ctx = (int)u32(a[CTRL_ATTR_FAMILY_ID]);
}

static int ready(void)
{
    double now = now_sec();
    if (g_fam > 0 && g_fd >= 0)
        return 1;
    if (now < g_retry_at)
        return 0;
    g_retry_at = now + 60;
    if (g_fd < 0)
        g_fd = nl_open();
    if (g_fd < 0)
        return 0;
    int fam = 0;
    if (ask(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, CTRL_ATTR_FAMILY_NAME, "nl80211", 8, family_cb, &fam) || fam <= 0) {
        g_fam = 0; /* no Wi-Fi in this kernel (yet) */
        return 0;
    }
    g_fam = fam;
    return 1;
}

/* ------------------------------------------------------------- answers */

static void iface_cb(const struct nlmsghdr *h, void *ctx)
{
    wifi_t *w = ctx;
    int len;
    const unsigned char *p = payload(h, &len);
    static span_t a[WF_ATTR_MAX];
    attrs(p, len, a, WF_ATTR_MAX);
    if (a[WF_ATTR_WIPHY_FREQ].len >= 4)
        w->freq = (int)u32(a[WF_ATTR_WIPHY_FREQ]);
    if (a[WF_ATTR_CENTER_FREQ1].len >= 4)
        w->center = (int)u32(a[WF_ATTR_CENTER_FREQ1]);
    if (a[WF_ATTR_CHANNEL_WIDTH].len >= 4) {
        static const int mhz[] = { 20, 20, 40, 80, 160, 160, 5, 10, 1, 2, 4, 8, 16, 320 };
        unsigned k = u32(a[WF_ATTR_CHANNEL_WIDTH]);
        w->width = k < sizeof mhz / sizeof mhz[0] ? mhz[k] : 0;
    }
}

/* a rate: Mb/s, and the Wi-Fi generation it belongs to (4 n, 5 ac, 6 ax, 7 be) */
static double rate(span_t s, int *gen)
{
    span_t r[WF_RATE_MAX];
    attrs(s.p, s.len, r, WF_RATE_MAX);
    double mbps = r[WF_RATE_BITRATE32].len >= 4 ? u32(r[WF_RATE_BITRATE32]) / 10.0
                : r[WF_RATE_BITRATE].len >= 2    ? u32(r[WF_RATE_BITRATE]) / 10.0
                                                  : -1;
    int g = r[WF_RATE_EHT_MCS].len ? 7 : r[WF_RATE_HE_MCS].len ? 6 : r[WF_RATE_VHT_MCS].len ? 5 : r[WF_RATE_MCS].len ? 4 : 0;
    if (g > *gen)
        *gen = g;
    return mbps;
}

static void station_cb(const struct nlmsghdr *h, void *ctx)
{
    wifi_t *w = ctx;
    int len;
    const unsigned char *p = payload(h, &len);
    static span_t a[WF_ATTR_MAX];
    attrs(p, len, a, WF_ATTR_MAX);
    if (!a[WF_ATTR_STA_INFO].len || w->sta) /* the access point: the first station */
        return;
    span_t si[WF_STA_MAX];
    attrs(a[WF_ATTR_STA_INFO].p, a[WF_ATTR_STA_INFO].len, si, WF_STA_MAX);
    w->sta = 1;
    if (si[WF_STA_SIGNAL].len >= 1)
        w->signal = (int8_t)si[WF_STA_SIGNAL].p[0];
    int gen = 0;
    if (si[WF_STA_TX_BITRATE].len)
        w->tx_mbps = rate(si[WF_STA_TX_BITRATE], &gen);
    if (si[WF_STA_RX_BITRATE].len)
        w->rx_mbps = rate(si[WF_STA_RX_BITRATE], &gen);
    w->gen = gen;
    if (si[WF_STA_TX_PACKETS].len >= 4)
        w->tx_pkts = u32(si[WF_STA_TX_PACKETS]);
    if (si[WF_STA_TX_RETRIES].len >= 4)
        w->tx_retries = u32(si[WF_STA_TX_RETRIES]);
    if (si[WF_STA_TX_FAILED].len >= 4)
        w->tx_failed = u32(si[WF_STA_TX_FAILED]);
    if (si[WF_STA_CONNECTED_TIME].len >= 4)
        w->connected_s = u32(si[WF_STA_CONNECTED_TIME]);
    if (si[WF_STA_BEACON_LOSS].len >= 4)
        w->beacon_loss = u32(si[WF_STA_BEACON_LOSS]);
}

static int fake_read(const char *dir, const char *ifname, const char *what, int dump, msg_fn fn, void *ctx)
{
    char p[PATHBUF];
    static char buf[32768] __attribute__((aligned(8)));
    sfmt(p, sizeof p, "%s/%s.%s", dir, ifname, what);
    FILE *f = fopen(p, "rbe");
    if (!f)
        return ENOENT;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int err = 0;
    return walk(buf, n, 0, dump, fn, ctx, &err) < 0 ? EIO : err;
}

/* ---------------------------------------------------------------- public */

int wifi_channel(int mhz)
{
    if (mhz == 2484)
        return 14;
    if (mhz >= 2412 && mhz < 2484)
        return (mhz - 2407) / 5;
    if (mhz == 5935) /* 6 GHz channel 2 sits apart */
        return 2;
    if (mhz >= 5955 && mhz <= 7115)
        return (mhz - 5950) / 5;
    if (mhz >= 5000 && mhz < 5925)
        return (mhz - 5000) / 5;
    if (mhz >= 4910 && mhz < 5000) /* 4.9 GHz (Japan) */
        return (mhz - 4000) / 5;
    if (mhz >= 58320 && mhz <= 70200)
        return (mhz - 56160) / 2160;
    return 0;
}

const char *wifi_band(int mhz)
{
    return mhz <= 0      ? ""
         : mhz < 3000    ? "2.4 GHz"
         : mhz < 5925    ? "5 GHz"
         : mhz < 7200    ? "6 GHz"
         : mhz >= 57000  ? "60 GHz"
                         : "";
}

void wifi_sample(netif_t *f, double mono)
{
    wifi_t *w = &f->wifi;
    wifi_t was = *w;
    const char *fake = secure_getenv("HSM_WIFI_FAKE");
    w->ok = w->sta = 0;
    w->freq = w->center = w->width = w->gen = w->signal = 0;
    w->tx_mbps = w->rx_mbps = -1;
    if (!f->up && !fake)
        return;
    if (fake) {
        if (fake_read(fake, f->name, "iface", 0, iface_cb, w) != 0)
            return;
        fake_read(fake, f->name, "station", 1, station_cb, w);
    } else {
        unsigned idx = if_nametoindex(f->name);
        if (!idx || !ready())
            return;
        int e = ask(g_fam, WF_CMD_GET_INTERFACE, 0, WF_ATTR_IFINDEX, &idx, 4, iface_cb, w);
        if (e == EAGAIN || e == EBADF || e == EIO) { /* no answer (EAGAIN is EWOULDBLOCK too): a fresh socket, in a minute */
            close(g_fd);
            g_fd = -1;
            g_fam = -1;
            g_retry_at = mono + 60;
            return;
        }
        if (e || !w->freq)
            return;
        ask(g_fam, WF_CMD_GET_STATION, NLM_F_DUMP, WF_ATTR_IFINDEX, &idx, 4, station_cb, w);
    }
    if (!w->freq || !w->sta) /* not connected */
        return;
    w->ok = 1;
    w->channel = wifi_channel(w->freq);
    /* the share of frames sent again, over the last 10 s or so */
    if (!w->win_t || w->tx_pkts < w->win_pkts || mono - w->win_t >= RETRY_EVERY) {
        if (w->win_t && w->tx_pkts >= w->win_pkts + 50 && w->tx_retries >= w->win_retries)
            w->retry_pct = 100.0 * (double)(w->tx_retries - w->win_retries) / (double)(w->tx_pkts - w->win_pkts);
        else if (w->win_t && w->tx_pkts < w->win_pkts + 50)
            w->retry_pct = -1; /* too little sent to say */
        w->win_t = mono;
        w->win_pkts = w->tx_pkts;
        w->win_retries = w->tx_retries;
    }
    if (!was.ok && !w->band_told) { /* the band it's on: told when it moves */
        w->band_told = w->freq;
        w->band_since = 0;
    }
}

/* "wlp2s0 moved from 5 GHz to 2.4 GHz": once it has stayed there 10 s */
int wifi_band_moved(netif_t *f, double mono, char *out, size_t n)
{
    wifi_t *w = &f->wifi;
    if (!w->ok || !w->band_told)
        return 0;
    if (!strcmp(wifi_band(w->freq), wifi_band(w->band_told))) {
        w->band_since = 0;
        return 0;
    }
    if (!w->band_since) {
        w->band_since = mono;
        return 0;
    }
    if (mono - w->band_since < BAND_SETTLE)
        return 0;
    int to24 = w->freq < 3000;
    sfmt(out, n, "%s moved from %s to %s (channel %d)%s", f->name, wifi_band(w->band_told), wifi_band(w->freq),
         w->channel, to24 ? ": slower and more crowded" : "");
    w->band_told = w->freq;
    w->band_since = 0;
    return 1;
}

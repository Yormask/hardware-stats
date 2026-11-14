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
/* ui_prim.c - drawing primitives shared by every view: text, boxes, bars,
 * graphs, and the colour / bar / graph styles chosen in Setup */
#define _GNU_SOURCE
#include "ui_int.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int U = 1;
const char *SEP = " | ";
const char *DEG = "C";
static layout_t g_default_lay;
const layout_t *g_lay = &g_default_lay;
static int g_colors;

const char *const SORT_NAMES[] = { "CPU%", "MEM%", "DISK I/O", "PID", "NAME", "TIME", "GPU%", "FILES" };
const char *const VIEW_NAMES[] = { "Dashboard", "Processes", "Hardware", "Setup",
                                   "Logging",   "Services",  "Events",   "History" };

static const char *const BLK[9] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static const char *const HB[8] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };


/* ================================================================ setup */

int ui_init(int utf8)
{
    U = utf8;
    SEP = U ? " · " : " | ";
    DEG = U ? "°C" : "C";
    if (!newterm(NULL, stdout, stdin)) /* unlike initscr(), lets us explain a bad $TERM */
        return -1;
    char *cup = tigetstr("cup");
    if (!cup || cup == (char *)-1) { /* "dumb": it can't put text where it belongs */
        endwin();
        return -2;
    }
    cbreak();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(100); /* tolerate escape sequences split by slow links (ssh) */
    g_colors = has_colors();
    if (g_colors) {
        start_color();
        use_default_colors();
    }
    return 0;
}

/* One value per column (NAN = no data), scaled lo..hi, drawn with the
 * eighth blocks whatever the graph style: the log viewer's graphs. */
void graph_cols(int y, int x, int h, int w, const float *v, double lo, double hi, attr_t a, int hl, attr_t ha)
{
    double span = hi - lo > 0 ? hi - lo : 1;
    for (int c = 0; c < w; c++) {
        if (isnan(v[c]))
            continue;
        attrset(c == hl ? ha : a);
        double f = CLAMP((v[c] - lo) / span, 0.0, 1.0);
        int lv = (int)(f * h * 8 + 0.5);
        if (lv == 0 && v[c] > lo)
            lv = 1; /* above the floor: show a sliver */
        for (int r = 0; r < h; r++) {
            int k = MIN(lv - r * 8, 8);
            if (k <= 0)
                break;
            mvaddstr(y + h - 1 - r, x + c, U ? BLK[k] : k >= 4 ? "|" : ".");
        }
    }
    attrset(A_NORMAL);
}

/* a paragraph over up to maxl lines, broken between words; returns the lines used */
int wrap_text(int y, int x, int w, int maxl, attr_t a, const char *t)
{
    int n = 0;
    while (*t && n < maxl && w > 0) {
        while (*t == ' ')
            t++;
        size_t nb = utf8_bytes_for_cols(t, w);
        if (t[nb] && nb) { /* break at the last space that fits */
            size_t b = nb;
            while (b > 0 && t[b] != ' ')
                b--;
            if (b > 0)
                nb = b;
        }
        if (!nb)
            break;
        put(y + n, x, w, a, "%.*s", (int)nb, t);
        t += nb;
        n++;
    }
    return n;
}

/* "key label" for a line of hints; clicking it presses the key. Returns
 * the columns used (0 when it doesn't fit before maxx). */
int key_hint(int y, int x, int maxx, const char *key, const char *label, int code)
{
    int need = utf8_cols(key) + utf8_cols(label) + 3;
    if (x + need > maxx)
        return 0;
    int w = put(y, x, maxx - x, A_BOLD, "%s", key);
    w += put(y, x + w, maxx - x - w, A_GRAY, " %s  ", label);
    if (code)
        hit_add(y, x, 1, w - 2, HIT_KEY, code, 0);
    return w;
}

/* ---------------------------------------------------------------- mouse */

void ui_mouse(int on)
{
#ifdef BUTTON5_PRESSED
    mmask_t m = BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED;
#else
    mmask_t m = BUTTON1_PRESSED | BUTTON4_PRESSED;
#endif
    mousemask(on ? m : 0, NULL);
    mouseinterval(0); /* report presses straight away; double clicks are timed by hand */
}

/* Clickable areas, registered while a frame is drawn. A click goes to the
 * last area added that contains it, so overlays (dialogs) win. */
#define MAX_HITS 1024
static hit_t g_hits[MAX_HITS];
static int g_nhits;

void hit_reset(void)
{
    g_nhits = 0;
}

void hit_add(int y, int x, int h, int w, int kind, int a, int b)
{
    if (g_nhits >= MAX_HITS || h <= 0 || w <= 0)
        return;
    g_hits[g_nhits++] = (hit_t){ y, x, h, w, kind, a, b };
}

const hit_t *hit_at(int y, int x)
{
    for (int i = g_nhits - 1; i >= 0; i--) {
        const hit_t *t = &g_hits[i];
        if (y >= t->y && y < t->y + t->h && x >= t->x && x < t->x + t->w)
            return t;
    }
    return NULL;
}

void ui_end(void)
{
    endwin();
}

/* =============================================================== colours */

/* All colours are the terminal's standard ones, so they follow whatever
 * colour theme the terminal itself uses. The user can pick the background,
 * the selection highlight and the "Single colour" from that same palette. */
static short g_bg = -1, g_fg = -1; /* background, and the text colour that suits it */
static int g_bright;               /* coloured dark background: use the bright text colours */
static int g_plain;                /* the same, but the terminal has only 8 colours */
/* bars (selection, header, keys, table headings) whose text is black: many
 * terminals (Konsole, xterm) show bold black as grey, which all but vanishes
 * on a green or cyan bar - so there bold is left out */
static unsigned char g_bar_black[64];

static short fit(short c) /* no bright colours: use the normal one */
{
    return (c >= 8 && COLORS < 16) ? (short)(c - 8) : c;
}

static int is_dark(short c) /* black, dark gray, blue, magenta, red: light text on top */
{
    return c == COLOR_BLACK || c == 8 || c == COLOR_BLUE || c == COLOR_MAGENTA || c == COLOR_RED;
}

/* coloured text on the background; text that would vanish into it uses the normal text colour */
static void text_pair(short n, short fg)
{
    if (g_bright && fg > COLOR_BLACK && fg < 8) /* cyan on blue etc. is hard to read */
        fg = (short)(fg + 8);
    if (g_plain && (fg == COLOR_CYAN || fg == COLOR_BLUE || fg == COLOR_MAGENTA || (fg == COLOR_RED && g_bg == COLOR_MAGENTA)))
        fg = COLOR_WHITE; /* no bright colours to switch to */
    fg = fit(fg);
    init_pair(n, (g_bg >= 0 && fg == g_bg) ? g_fg : fg, g_bg);
}

/* a solid bar (selection, header, key hint) with text in colour fg (-1: black or
 * white, whichever reads better); turned inside out if it matches the background */
static void block_pair(short n, short bg, short fg)
{
    bg = fit(bg);
    int chosen = fg >= 0;
    if (chosen)
        fg = fit(fg);
    if (g_bg >= 0 && bg == g_bg) { /* the bar would vanish: draw it in the screen's text colour */
        short tf = (chosen && fg != g_fg) ? fg : bg;
        init_pair(n, tf, g_fg);
        if (n >= 0 && n < 64)
            g_bar_black[n] = tf == COLOR_BLACK;
        return;
    }
    if (!chosen || fg == bg) /* text the same colour as the bar would vanish too */
        fg = is_dark(bg) ? (COLORS >= 16 ? 15 : COLOR_WHITE) : COLOR_BLACK;
    init_pair(n, fg, bg);
    if (n >= 0 && n < 64)
        g_bar_black[n] = fg == COLOR_BLACK;
}

static void init_colors(const layout_t *l)
{
    g_bg = fit(BG_COLOR[CLAMP(l->background, 0, BG_COUNT - 1)]);
    int dark = g_bg >= 0 && is_dark(g_bg), coloured = dark && g_bg != COLOR_BLACK;
    g_bright = coloured && COLORS >= 16;
    g_plain = coloured && COLORS < 16;
    /* text: the terminal's own on its own background, light on dark, black on light */
    g_fg = g_bg < 0 ? -1 : !dark ? COLOR_BLACK : g_bright ? 15 : COLOR_WHITE;
    /* secondary text, borders: dark gray normally, a lighter shade on coloured backgrounds */
    short gray = coloured ? COLOR_WHITE : COLORS >= 16 ? 8 : COLOR_WHITE;
    short border = !coloured ? (COLORS >= 16 ? 8 : COLOR_BLUE) : g_bright && g_bg < 8 ? (short)(g_bg + 8) : COLOR_WHITE;
    if (border == g_bg)
        border = g_fg;
    short hl = SINGLE_COLOR[CLAMP(l->highlight, 0, SINGLE_COUNT - 1)];
    short st = TXT_COLOR[CLAMP(l->sel_text, 0, TXT_COUNT - 1)];
    short tt = TXT_COLOR[CLAMP(l->title_color, 0, TXT_COUNT - 1)];
    text_pair(C_TITLE, tt < 0 ? COLOR_CYAN : tt);
    init_pair(C_BORDER, border, g_bg);
    text_pair(C_OK, COLOR_GREEN);
    text_pair(C_WARN, COLOR_YELLOW);
    text_pair(C_CRIT, COLOR_RED);
    init_pair(C_GRAY, gray, g_bg);
    block_pair(C_HDR, hl, st);
    block_pair(C_THEAD, COLOR_GREEN, -1);
    block_pair(C_SEL, hl, st);
    text_pair(C_ACC, COLOR_MAGENTA);
    text_pair(C_INFO, COLOR_CYAN);
    block_pair(C_KEY, hl, st);
    text_pair(C_IN, COLOR_CYAN);
    text_pair(C_OUT, COLOR_MAGENTA);
    text_pair(C_BLO, COLOR_GREEN);
    text_pair(C_BMID, COLOR_YELLOW);
    text_pair(C_BHI, COLOR_RED);
    init_pair(C_TRACK, gray, g_bg);
    text_pair(C_GLO, COLOR_GREEN);
    text_pair(C_GMID, COLOR_YELLOW);
    text_pair(C_GHI, COLOR_RED);
    text_pair(C_SINGLE, SINGLE_COLOR[CLAMP(l->single_color, 0, SINGLE_COUNT - 1)]);
    init_pair(C_BASE, g_fg, g_bg);
    bkgd(COLOR_PAIR(C_BASE) | ' '); /* plain text and empty space take the background */
}

/* (re)build the colour pairs when the colour settings change */
void colors_sync(const layout_t *l)
{
    static int last[5] = { -1 };
    if (!g_colors)
        return;
    int key[5] = { l->single_color, l->highlight, l->background, l->sel_text, l->title_color };
    if (!memcmp(key, last, sizeof key))
        return;
    memcpy(last, key, sizeof key);
    init_colors(l);
}

static int mono(void)
{
    return !g_colors || g_lay->color_mode == COLOR_MONO;
}

int gradient_on(void)
{
    return !mono() && g_lay->color_mode == COLOR_GRADIENT;
}

attr_t cp(int n)
{
    if (mono()) /* keep bars/highlights readable without colour */
        return (n == C_HDR || n == C_THEAD || n == C_SEL || n == C_KEY) ? A_REVERSE : 0;
    if (g_lay->color_mode == COLOR_SINGLE && (n == C_IN || n == C_OUT)) /* clock bars, I/O graphs */
        return COLOR_PAIR(C_SINGLE);
    return COLOR_PAIR(n);
}

attr_t gray_attr(void)
{
    if (mono())
        return A_DIM;
    return COLOR_PAIR(C_GRAY) | (COLORS < 16 ? A_DIM : 0);
}

/* text colours (values, statuses) */
attr_t pct_attr(double p)
{
    if (mono())
        return p >= 85 ? A_BOLD : A_NORMAL;
    if (g_lay->color_mode == COLOR_SINGLE)
        return COLOR_PAIR(C_SINGLE);
    return COLOR_PAIR(p >= 85 ? C_CRIT : p >= 60 ? C_WARN : C_OK);
}

attr_t temp_attr(double t)
{
    if (mono())
        return t >= 85 ? A_BOLD : A_NORMAL;
    if (g_lay->color_mode == COLOR_SINGLE)
        return COLOR_PAIR(C_SINGLE);
    return COLOR_PAIR(t >= 85 ? C_CRIT : t >= 70 ? C_WARN : C_OK);
}

/* bar and graph colours */
attr_t bar_attr(double p)
{
    if (mono())
        return p >= 85 ? A_BOLD : A_NORMAL;
    if (g_lay->color_mode == COLOR_SINGLE)
        return COLOR_PAIR(C_SINGLE);
    return COLOR_PAIR(p >= 85 ? C_BHI : p >= 60 ? C_BMID : C_BLO);
}

attr_t bar_temp_attr(double t)
{
    return bar_attr(t >= 85 ? 100 : t >= 70 ? 70 : 0);
}

/* with your own limit: red from the limit, yellow from 10 below it */
static double lim_scale(double t, double lim)
{
    return lim <= 0 ? t : t >= lim ? 85 : t >= lim - 10 ? 70 : 0;
}

attr_t temp_attr_lim(double t, double lim)
{
    return temp_attr(lim_scale(t, lim));
}

attr_t bar_temp_attr_lim(double t, double lim)
{
    return bar_temp_attr(lim_scale(t, lim));
}

attr_t graph_attr(double p)
{
    if (mono())
        return p >= 85 ? A_BOLD : A_NORMAL;
    if (g_lay->color_mode == COLOR_SINGLE)
        return COLOR_PAIR(C_SINGLE);
    return COLOR_PAIR(p >= 85 ? C_GHI : p >= 60 ? C_GMID : C_GLO);
}

/* ================================================================== text */

/* bold black text on a bar turns grey in many terminals: plain black instead */
static attr_t legible(attr_t a)
{
    if (a & A_BOLD) {
        int pn = (int)PAIR_NUMBER(a);
        if (pn > 0 && pn < 64 && g_bar_black[pn])
            a &= ~(attr_t)A_BOLD;
    }
    return a;
}

int put(int y, int x, int maxw, attr_t a, const char *fmt, ...)
{
    if (maxw <= 0 || y < 0 || y >= LINES || x < 0 || x >= COLS)
        return 0;
    a = legible(a);
    if (x + maxw > COLS)
        maxw = COLS - x;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[utf8_bytes_for_cols(buf, maxw)] = '\0';
    attrset(a);
    mvaddstr(y, x, buf);
    attrset(A_NORMAL);
    return utf8_cols(buf);
}

void app(char *b, size_t n, const char *fmt, ...)
{
    size_t l = strlen(b);
    if (l + 1 >= n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + l, n - l, fmt, ap);
    va_end(ap);
}

void fill(int y, int x, int w, attr_t a)
{
    if (y < 0 || y >= LINES || x >= COLS)
        return;
    if (x + w > COLS)
        w = COLS - x;
    if (w > 0)
        mvhline(y, x, ' ' | legible(a), w);
}

void kv(int y, int *x, int xmax, attr_t va, const char *key, const char *fmt, ...)
{
    char v[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    int need = (*key ? utf8_cols(key) + 1 : 0) + utf8_cols(v);
    if (*x + need > xmax) {
        *x = xmax;
        return;
    }
    if (*key)
        *x += put(y, *x, xmax - *x, A_GRAY, "%s ", key);
    *x += put(y, *x, xmax - *x, va, "%s", v);
    *x += 2;
}

/* ================================================================= boxes */

void box_(int y, int x, int h, int w, const char *title, const char *info)
{
    if (h < 2 || w < 4)
        return;
    attrset(CP(C_BORDER));
    if (U) {
        mvaddstr(y, x, "╭");
        for (int i = 1; i < w - 1; i++)
            addstr("─");
        addstr("╮");
        for (int r = 1; r < h - 1; r++) {
            mvaddstr(y + r, x, "│");
            mvaddstr(y + r, x + w - 1, "│");
        }
        mvaddstr(y + h - 1, x, "╰");
        for (int i = 1; i < w - 1; i++)
            addstr("─");
        addstr("╯");
    } else { /* plain ASCII works on every terminal and serial console */
        mvaddch(y, x, '+');
        mvhline(y, x + 1, '-', w - 2);
        mvaddch(y, x + w - 1, '+');
        mvvline(y + 1, x, '|', h - 2);
        mvvline(y + 1, x + w - 1, '|', h - 2);
        mvaddch(y + h - 1, x, '+');
        mvhline(y + h - 1, x + 1, '-', w - 2);
        mvaddch(y + h - 1, x + w - 1, '+');
    }
    attrset(A_NORMAL);
    int tx = x + 2, lim = x + w - 2;
    if (title)
        tx += put(y, tx, lim - tx, CP(C_TITLE) | A_BOLD, " %s ", title);
    if (info && *info)
        put(y, tx, lim - tx, A_GRAY, "%s ", info);
}

/* ================================================================== bars */

/* Horizontal bar in the style picked in Setup. a == 0 colours by value
 * (or per cell in gradient mode); a != 0 forces that colour. */
void bar(int y, int x, int w, double pct, attr_t a)
{
    if (w <= 0 || y < 0 || y >= LINES || x >= COLS)
        return;
    if (x + w > COLS)
        w = COLS - x;
    pct = CLAMP(pct, 0.0, 100.0);
    int style = U ? g_lay->bar_style : BAR_CLASSIC;
    int grad = !a && gradient_on();
    if (!a)
        a = bar_attr(pct);
    attr_t track = mono() ? A_DIM : COLOR_PAIR(C_TRACK) | (COLORS < 16 ? A_DIM : 0);

    const char *on = "█", *off = "·";
    int bx = x, bw = w;
    if (style == BAR_CLASSIC) {
        if (w >= 4) { /* [|||||     ] */
            attrset(A_GRAY);
            mvaddstr(y, x, "[");
            mvaddstr(y, x + w - 1, "]");
            bx = x + 1;
            bw = w - 2;
        }
        on = "|";
        off = " ";
    } else if (style == BAR_SQUARES) {
        on = off = "■";
    } else if (style == BAR_LINE) {
        on = "━";
        off = "─";
    } else if (!U) {
        on = "|";
        off = ".";
    }

    double cells = pct / 100.0 * bw;
    int full, part = 0;
    if (style == BAR_BLOCKS && U) {
        full = (int)cells;
        part = (int)((cells - full) * 8);
    } else {
        full = (int)(cells + 0.5);
    }
    full = MIN(full, bw);
    move(y, bx);
    for (int i = 0; i < full; i++) {
        attrset(grad ? bar_attr(100.0 * (i + 1) / bw) : a);
        addstr(on);
    }
    int used = full;
    if (part > 0 && used < bw) {
        attrset(grad ? bar_attr(100.0 * (used + 1) / bw) : a);
        addstr(HB[part]);
        used++;
    }
    attrset(track);
    for (; used < bw; used++)
        addstr(off);
    attrset(A_NORMAL);
}

/* "Label [bar...........] value" */
void meter(int y, int x, int w, const char *label, int lw, double pct, const char *val, int vw, attr_t a)
{
    int vc = utf8_cols(val);
    if (vw < vc)
        vw = vc;
    put(y, x, lw, CP(C_INFO), "%s", label);
    int bw = w - lw - vw - 1;
    if (bw > 0)
        bar(y, x + lw, bw, pct, a);
    put(y, x + w - vw, vw, A_BOLD, "%*s", vw, val);
}

/* ================================================================ graphs */

static int braille_on(void)
{
    return U && g_lay->graph_style == GRAPH_BRAILLE;
}

int graph_span(int w)
{
    return braille_on() ? 2 * w : w;
}

static int levels(double v, double maxv, int steps)
{
    if (v <= 0)
        return 0;
    int l = (int)(CLAMP(v / maxv, 0.0, 1.0) * steps + 0.5);
    return l ? l : 1;
}

static void braille_at(int y, int x, int bits)
{
    unsigned c = 0x2800u + (unsigned)bits;
    char b[4] = { (char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)), 0 };
    mvaddstr(y, x, b);
}

/* History graph, newest sample on the right. fixed==0 -> coloured by
 * height. flip draws downward from the top (used for "out" series).
 * Braille packs 2 samples x 4 levels into every character cell. */
void graph(int y, int x, int h, int w, const hist_t *hs, double maxv, attr_t fixed, int flip)
{
    if (h <= 0 || w <= 0 || g_lay->graph_style == GRAPH_OFF)
        return;
    if (maxv <= 0)
        maxv = 1;
    static const int LB[4] = { 0x40, 0x04, 0x02, 0x01 }; /* left dots, bottom -> top  */
    static const int RB[4] = { 0x80, 0x20, 0x10, 0x08 }; /* right dots, bottom -> top */
    int br = braille_on();
    for (int c = 0; c < w; c++) {
        double v, v2 = -1;
        int lv, lv2 = 0, steps = h * (br ? 4 : 8);
        if (br) {
            v2 = hist_get(hs, 2 * (w - 1 - c));  /* newer sample: right dots */
            v = hist_get(hs, 2 * (w - 1 - c) + 1); /* older sample: left dots */
            lv2 = levels(v2, maxv, steps);
        } else {
            v = hist_get(hs, w - 1 - c);
        }
        lv = levels(v, maxv, steps);
        double vmax = MAX(v, v2);
        for (int r = 0; r < h; r++) {
            attr_t a = fixed ? fixed : h <= 2 ? graph_attr(100.0 * vmax / maxv) : graph_attr(100.0 * (r + 0.5) / h);
            int yy = flip ? y + r : y + h - 1 - r;
            if (br) {
                int fl = CLAMP(lv - r * 4, 0, 4), fr = CLAMP(lv2 - r * 4, 0, 4), bits = 0;
                if (!fl && !fr)
                    break;
                for (int k = 0; k < fl; k++)
                    bits |= flip ? LB[3 - k] : LB[k];
                for (int k = 0; k < fr; k++)
                    bits |= flip ? RB[3 - k] : RB[k];
                attrset(a);
                braille_at(yy, x + c, bits);
                continue;
            }
            int f = MIN(lv - r * 8, 8);
            if (f <= 0)
                break;
            if (!U) {
                attrset(a);
                mvaddstr(yy, x + c, f >= 4 ? "|" : ".");
            } else if (flip) { /* reverse video turns a lower block into an upper one */
                attrset(a | A_REVERSE);
                mvaddstr(yy, x + c, BLK[8 - f]);
            } else {
                attrset(a);
                mvaddstr(yy, x + c, BLK[f]);
            }
        }
    }
    attrset(A_NORMAL);
}

/* "R" graph growing up, "W" graph hanging down, sharing one scale. */
void dual_graph(int y, int x, int h, int w, const hist_t *up, const hist_t *dn, attr_t ua, attr_t da, const char *ul,
                const char *dl)
{
    if (h <= 0 || w <= 3 || g_lay->graph_style == GRAPH_OFF)
        return;
    int gx = x + 2, gw = w - 2, span = graph_span(gw);
    double mx = MAX(hist_max(up, span), hist_max(dn, span)) * 1.05;
    if (mx <= 0)
        mx = 1;
    if (h == 1) {
        graph(y, gx, 1, gw, up, mx, ua, 0);
        put(y, x, 1, ua | A_BOLD, "%s", ul);
        return;
    }
    int uh = (h + 1) / 2, dh = h - uh;
    graph(y, gx, uh, gw, up, mx, ua, 0);
    graph(y + uh, gx, dh, gw, dn, mx, da, 1);
    put(y, x, 1, ua | A_BOLD, "%s", ul);
    put(y + h - 1, x, 1, da | A_BOLD, "%s", dl);
    if (h >= 3) {
        char m[16];
        fmt_rate(mx, m, sizeof m);
        put(y, gx, gw, A_GRAY, "%s", m);
    }
}

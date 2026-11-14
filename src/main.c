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

/* main.c - argument parsing and the main loop */
#define _GNU_SOURCE
#include "cooling.h"
#include "log.h"
#include "serve.h"
#include "ui.h"
#include "watch.h"

#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sys_t S; /* big (history buffers, per-CPU arrays): keep it off the stack */
static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
            "%s (%s) %s\nLive monitor for CPU, GPU, memory, power, disk, network, services and system health.\n\n"
            "Usage: %s [options]\n"
            "  -d, --delay SEC   refresh interval in seconds (default: saved setting or 1)\n"
            "  -1, --once        print one plain-text snapshot and exit\n"
            "  -j, --json        print one snapshot as JSON (for scripts) and exit\n"
            "  -n, --top N       processes listed by --once (default 15) or --json (default all)\n"
            "  --view FILE       open a saved log in the log viewer\n"
            "  -a, --ascii       draw with plain ASCII instead of Unicode\n"
            "  -h, --help        show this help\n"
            "  -v, --version     show version\n\n"
            "Without the screen (runs until Ctrl+C or kill; these can be combined):\n"
            "  --watch           flight recorder, alerts, freeze and crash detection, and the\n"
            "                    events and daily history (views 7 and 8)\n"
            "  --log             log the values chosen in the Logging view (5) to a CSV file\n"
            "  --log-pid PID     log one process's usage (can be given up to %d times)\n"
            "  --log-every SEC   seconds between rows (default: the saved setting)\n"
            "  --log-dir DIR     where to write the files (default: %s)\n"
            "  --serve [ADDR:]PORT  answer over HTTP: /metrics (Prometheus), /json; only this\n"
            "                    machine unless ADDR says otherwise (0.0.0.0:9101: everyone)\n\n"
            "  --cooling-check   a minute of full CPU load: does the cooling keep up? (exit 1: no)\n\n"
            "Keys: 1-8 views (6 services, 7 events, 8 history), Enter process details, s setup,\n"
            "      l log sensors, L log the selected process, x kill, N priority (nice),\n"
            "      T process tree, c/m/d/g/o/p/n/t sort, / filter, b CPU core bars,\n"
            "      v (in view 5) saved logs, +/- interval, space pause, ? help, q quit\n"
            "      The mouse works too: tabs, rows, column headings, the wheel.\n\n"
            "Settings are saved in %s\n\n"
            "Run as root (sudo %s) to also see CPU power (RAPL), exact Intel GPU load,\n"
            "the kernel log, NVMe and SATA drive health and other users' processes.\n\n"
            "%s. This program comes with ABSOLUTELY NO WARRANTY.\n"
            "It is free software under the GNU GPL v3 or later; see the LICENSE file.\n",
            APP_NAME, APP_LONG_NAME, APP_VERSION, APP_NAME, MAX_PLOG, config_log_dir(NULL), config_path(),
            APP_NAME, APP_COPYRIGHT);
}

static void sleep_sec(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    while (nanosleep(&ts, &ts) == -1 && !g_quit)
        ;
}

static void print_note(void)
{
    char m[200];
    int err;
    while (log_note_pop(m, sizeof m, &err))
        fprintf(err ? stderr : stdout, "%s: %s\n", APP_NAME, m);
    fflush(stdout);
}

static const char *pretty(const char *path)
{
    static char b[PATHBUF];
    config_pretty_path(path, b, sizeof b);
    return b;
}

static void print_notice(void)
{
    if (!g_notice.n)
        return;
    printf("%s\n", g_notice.title);
    for (int i = 0; i < g_notice.n; i++)
        printf("  %s\n", g_notice.line[i]);
    fflush(stdout);
}

/* hsm --log / --log-pid / --watch / --serve: record (or answer) without drawing anything */
static int headless(ui_state_t *u, int sys_log, const pid_t *pids, int npid, double every, const char *dir, int watch,
                    const char *serve)
{
    char err[PATHBUF + 64];
    if (dir) {
        if (dir[0] == '/' || dir[0] == '~') {
            str_copy(u->log_dir, sizeof u->log_dir, dir);
        } else {
            char cwd[PATHBUF];
            if (!getcwd(cwd, sizeof cwd)) {
                fprintf(stderr, "%s: %s\n", APP_NAME, strerror(errno));
                return 1;
            }
            sfmt(u->log_dir, sizeof u->log_dir, "%s/%s", cwd, dir);
        }
    }
    int sfd = -1;
    if (serve) {
        char shown[160] = "";
        sfd = serve_open(serve, err, sizeof err, shown, sizeof shown);
        if (sfd < 0) {
            fprintf(stderr, "%s: --serve: %s\n", APP_NAME, err);
            return 1;
        }
        printf("Serving on http://%s/  (/metrics for Prometheus, /json)\n", shown);
        if (strncmp(shown, "127.", 4) && strncmp(shown, "[::1]", 5))
            printf("Anyone who can reach this address can read the figures: there is no password.\n");
    }
    if (every <= 0)
        every = u->log_every > 0 ? u->log_every : u->interval;
    /* watching needs a sample a second (recorder, alerts); logs keep their own pace */
    double sample = watch || serve ? MIN(every, 1.0) : every;
    str_copy(g_log.dir, sizeof g_log.dir, config_log_dir(u));
    g_log.every = every > sample + 1e-9 ? every : 0; /* 0: a row on every sample */

    sleep_sec(0.25);
    sys_sample(&S);
    netq_set(u->net_check, u->net_check_host); /* the network check, if it's on: its columns too */
    netq_read(&S.net.q);
    log_build_channels(&S, u->log_skip);
    if (watch) {
        u->rec_on = 1; /* that's what --watch is for, whatever the screen's setting */
        watch_start(&S, u, sys_log || npid ? "log+watch" : "watch", 0);
        print_notice();
        print_note();
        char p[PATHBUF];
        config_pretty_path(config_log_dir(u), p, sizeof p);
        printf("Watching: flight recorder (the last %d minutes), alerts (%s), crash detection,\n"
               "freezes, events and daily history. Incident logs go to %s\n",
               u->rec_minutes, (const char *const[]){ "off", "desktop", "bell", "desktop + bell" }[u->alert_notify], p);
    }
    if (sys_log) {
        if (log_start(&S, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", APP_NAME, err);
            return 1;
        }
        printf("Logging %d values every %g s to %s\n", log_count_on(), every, g_log.path);
    }
    for (int i = 0; i < npid; i++) {
        proc_t *p = NULL;
        for (int k = 0; k < S.procs.n && !p; k++)
            if (S.procs.p[k].pid == pids[i])
                p = &S.procs.p[k];
        if (!p) {
            fprintf(stderr, "%s: no process with PID %d\n", APP_NAME, (int)pids[i]);
            continue;
        }
        if (log_proc_start(&S, p->pid, p->start, err, sizeof err) != 0)
            fprintf(stderr, "%s: %s\n", APP_NAME, err);
        else
            printf("Logging PID %d (%s) every %g s to %s\n", (int)p->pid, p->comm, every,
                   g_log.pl[g_log.nplog - 1].path);
    }
    if (!watch && sfd < 0 && !g_log.f && !g_log.nplog) {
        log_close_all();
        return 1;
    }
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);

    double next = now_sec() + sample;
    while (!g_quit && (watch || sfd >= 0 || g_log.f || g_log.nplog)) {
        double d = next - now_sec();
        if (d > 0 && sfd >= 0)
            serve_wait(sfd, d, &S, &g_quit); /* answers requests while it waits */
        else if (d > 0)
            sleep_sec(d);
        if (g_quit)
            break;
        sys_sample(&S);
        log_tick(&S);
        if (watch)
            watch_tick(&S, u);
        print_note();
        next += sample;
        if (next < now_sec()) /* fell behind (suspend, slow disk): don't try to catch up */
            next = now_sec() + sample;
    }
    if (watch)
        watch_end(&S);
    netq_set(0, NULL);
    if (sfd >= 0)
        close(sfd);
    print_note();
    if (g_log.f)
        printf("Saved %lu rows to %s\n", g_log.rows, pretty(g_log.path));
    for (int i = 0; i < g_log.nplog; i++)
        printf("Saved %lu rows to %s\n", g_log.pl[i].rows, pretty(g_log.pl[i].path));
    log_close_all();
    return 0;
}

int main(int argc, char **argv)
{
    double interval = -1; /* -1: use the saved setting */
    int once = 0, json = 0, ascii = 0, ntop = -1, watch = 0, cooling = 0;
    const char *serve = NULL;
    int sys_log = 0, npid = 0;
    pid_t pids[MAX_PLOG];
    double log_every = -1;
    const char *log_dir = NULL, *view = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-d") || !strcmp(a, "--delay")) && i + 1 < argc) {
            interval = atof(argv[++i]);
        } else if (!strncmp(a, "--delay=", 8)) {
            interval = atof(a + 8);
        } else if (!strcmp(a, "-1") || !strcmp(a, "--once")) {
            once = 1;
        } else if (!strcmp(a, "-j") || !strcmp(a, "--json")) {
            json = 1;
        } else if (!strcmp(a, "--watch")) {
            watch = 1;
        } else if (!strcmp(a, "--cooling-check")) {
            cooling = 1;
        } else if (!strcmp(a, "--serve") && i + 1 < argc) {
            serve = argv[++i];
        } else if (!strncmp(a, "--serve=", 8)) {
            serve = a + 8;
        } else if (!strcmp(a, "--view") && i + 1 < argc) {
            view = argv[++i];
        } else if ((!strcmp(a, "-n") || !strcmp(a, "--top")) && i + 1 < argc) {
            ntop = atoi(argv[++i]);
        } else if (!strcmp(a, "-a") || !strcmp(a, "--ascii")) {
            ascii = 1;
        } else if (!strcmp(a, "--log")) {
            sys_log = 1;
        } else if (!strcmp(a, "--log-pid") && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v <= 0 || v > 0x3fffffff) {
                fprintf(stderr, "%s: '%s' is not a process ID\n", APP_NAME, argv[i]);
                return 2;
            }
            if (npid >= MAX_PLOG) {
                fprintf(stderr, "%s: at most %d --log-pid options\n", APP_NAME, MAX_PLOG);
                return 2;
            }
            pids[npid++] = (pid_t)v;
        } else if (!strcmp(a, "--log-every") && i + 1 < argc) {
            log_every = atof(argv[++i]);
            if (log_every < 0.1 || log_every > 86400) {
                fprintf(stderr, "%s: --log-every must be between 0.1 and 86400 seconds\n", APP_NAME);
                return 2;
            }
        } else if (!strcmp(a, "--log-dir") && i + 1 < argc) {
            log_dir = argv[++i];
            if (!*log_dir || strlen(log_dir) >= PATHBUF - 128) {
                fprintf(stderr, "%s: bad --log-dir\n", APP_NAME);
                return 2;
            }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("%s %s\n%s\n"
                   "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
                   "This is free software: you are free to change and redistribute it.\n"
                   "There is NO WARRANTY, to the extent permitted by law.\n",
                   APP_NAME, APP_VERSION, APP_COPYRIGHT);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n\n", APP_NAME, a);
            usage(stderr);
            return 2;
        }
    }
    if (interval != -1 && interval < 0.1)
        interval = 0.1;
    if (interval > 60)
        interval = 60;

    /* Test hooks. secure_getenv() ignores them when the program runs setuid or with
     * file capabilities, so they can't be used to read arbitrary files. */
    const char *e;
    if ((e = secure_getenv("HSM_SYSFS")) && *e)
        g_sysfs = e;
    if ((e = secure_getenv("HSM_PROCFS")) && *e)
        g_procfs = e;
    if ((e = secure_getenv("HSM_DEVFS")) && *e)
        g_devfs = e;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    struct sigaction old;
    if (sigaction(SIGHUP, NULL, &old) == 0 && old.sa_handler != SIG_IGN) /* keep nohup working */
        sigaction(SIGHUP, &sa, NULL);
    /* output into a pipe whose reader went away (hsm --watch | head) must not
     * kill hsm before it cleans up: writes just fail instead */
    signal(SIGPIPE, SIG_IGN);

    setlocale(LC_ALL, "");
    /* numbers stay "12.5" in logs, JSON and the config file, also where the
     * language writes "12,5" (a comma would break the CSV columns) */
    setlocale(LC_NUMERIC, "C");
    int utf8 = !ascii && !strcmp(nl_langinfo(CODESET), "UTF-8");

    sys_init(&S);

    if (cooling) { /* a minute of full load, then the verdict */
        static ui_state_t cu;
        config_defaults(&cu);
        config_load(&cu);
        sensors_apply(&S.sens, cu.srule, cu.nsrule);
        sleep_sec(0.5);
        sys_sample(&S);
        int r = cooling_cli(&S, &g_quit);
        sys_free(&S);
        return r;
    }
    if (once || json) {
        static ui_state_t ou; /* only for your sensor names */
        config_defaults(&ou);
        config_load(&ou);
        sensors_apply(&S.sens, ou.srule, ou.nsrule);
        S.svc.want_until = now_sec() + 60; /* services too */
        svcs_sample(&S.svc);
        sleep_sec(interval > 0 ? interval : 1.0);
        sys_sample(&S);
        if (json)
            json_print(&S, ntop);
        else
            report_print(&S, ntop >= 0 ? ntop : 15);
        sys_free(&S);
        return 0;
    }
    if (sys_log || npid || watch || serve) {
        static ui_state_t hu;
        config_defaults(&hu);
        config_load(&hu);
        sensors_apply(&S.sens, hu.srule, hu.nsrule);
        if (interval > 0)
            hu.interval = interval;
        int r = headless(&hu, sys_log, pids, npid, log_every, log_dir, watch, serve);
        sys_free(&S);
        return r;
    }
    char view_path[PATHBUF] = "";
    if (view) {
        char *rp = realpath(view, NULL);
        if (!rp || strlen(rp) >= sizeof view_path) {
            fprintf(stderr, "%s: %s: %s\n", APP_NAME, view, rp ? "path too long" : strerror(errno));
            free(rp);
            sys_free(&S);
            return 1;
        }
        str_copy(view_path, sizeof view_path, rp);
        free(rp);
    }
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s: stdout is not a terminal (use --once for a text snapshot)\n", APP_NAME);
        sys_free(&S);
        return 1;
    }

    sleep_sec(0.25); /* short warm-up so the first frame already shows rates */
    sys_sample(&S);

    static ui_state_t u;
    config_defaults(&u);
    config_load(&u); /* ~/.config/hsm/config, if it exists */
    sensors_apply(&S.sens, u.srule, u.nsrule);
    if (interval > 0)
        u.interval = interval;
    watch_start(&S, &u, "screen", 1); /* how did the last session end? (and the logs' columns) */
    u.notice_open = g_notice.n > 0;
    int ui = ui_init(utf8);
    if (ui != 0) {
        const char *t = getenv("TERM");
        if (ui == -2)
            fprintf(stderr,
                    "%s: this terminal ('%s') can't move the cursor, so a screen like hsm's can't be drawn on it.\n"
                    "Run it in a terminal window, or use '%s --once' for a text snapshot.\n",
                    APP_NAME, t ? t : "", APP_NAME);
        else
            fprintf(stderr,
                    "%s: unknown terminal type '%s'.\n"
                    "Try:  TERM=xterm-256color %s\n"
                    "(or install your distribution's ncurses-term package)\n",
                    APP_NAME, t ? t : "", APP_NAME);
        watch_end(&S);
        sys_free(&S);
        return 1;
    }
    ui_mouse(u.lay.mouse);
    if (view_path[0])
        lv_open(&u, view_path);

    double next = now_sec() + u.interval;
    while (!g_quit) {
        ui_draw(&S, &u);
        double now = now_sec();
        int ms = (int)((next - now) * 1000.0);
        timeout(ms < 0 ? 0 : ms);
        int ch = getch();
        if (ch == 27) { /* a lone Esc, or the start of a key sequence we don't know? */
            nodelay(stdscr, TRUE);
            int c2 = getch();
            if (c2 != ERR) { /* more bytes were already waiting: swallow the sequence */
                if (c2 == '[' || c2 == 'O')
                    for (int c3, k = 0; (c3 = getch()) != ERR && k < 16; k++)
                        if (c3 >= 0x40 && c3 <= 0x7e)
                            break;
                ch = ERR;
            }
            nodelay(stdscr, FALSE);
        }
        if (ch != ERR && ch != KEY_RESIZE && !ui_key(&S, &u, ch))
            break;
        now = now_sec();
        if (next > now + u.interval) /* interval was shortened */
            next = now + u.interval;
        if (now >= next) {
            if (!u.paused) {
                sys_sample(&S);
                log_tick(&S);
                watch_tick(&S, &u);
            } else {
                session_tick(); /* paused, but still alive */
            }
            next = now + u.interval;
        }
    }

    ui_end();
    watch_end(&S);
    if (g_log.f)
        printf("Log saved: %s (%lu rows)\n", pretty(g_log.path), g_log.rows);
    for (int i = 0; i < g_log.nplog; i++)
        printf("Log saved: %s (%lu rows)\n", pretty(g_log.pl[i].path), g_log.pl[i].rows);
    log_close_all();
    if (config_changed(&u)) /* remember sort order, interval, CPU bar mode... */
        config_save(&u);
    sys_free(&S);
    return 0;
}

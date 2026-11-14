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

/* collect.h - data collectors for every subsystem hsm monitors.
 *
 * Every collector reads directly from /proc and /sys (no external tools, no
 * helper processes), keeps the previous raw counters itself and turns them
 * into rates using the real elapsed time `dt` passed in by the caller. */
#ifndef HSM_COLLECT_H
#define HSM_COLLECT_H

#include "util.h"
#include <sys/types.h>

/* ------------------------------------------------------------------ CPU */
#define MAX_CPUS 512

typedef struct {
    double user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_jiffies_t;

#define MAX_CSTATES 12
typedef struct {
    char name[16];      /* "C1", "C6", "POLL" */
    double pct;         /* average share of time all CPUs spent in it */
    unsigned latency;   /* exit latency in us */
    int disabled;
} cstate_t;

typedef struct {
    char model[128];
    int cores, threads;   /* topology from /proc/cpuinfo                   */
    int ncpu;             /* CPUs currently online (lines in /proc/stat)   */
    int id[MAX_CPUS];     /* kernel CPU number of each entry               */
    double pct[MAX_CPUS]; /* busy % per logical CPU                        */
    double mhz[MAX_CPUS]; /* current clock per logical CPU (0 = unknown)   */
    /* per kernel CPU number (index = id[]): hardware max clock, topology */
    double mhz_hw[MAX_CPUS];
    int core_id[MAX_CPUS], pkg_id[MAX_CPUS];
    unsigned char topo_done[MAX_CPUS];
    double total, user, sys, iowait, steal;
    double mhz_avg, mhz_max;
    double temp_c; /* package temperature, -1 = unknown */
    double load[3], uptime;
    unsigned procs_running, procs_blocked;
    double ctxt_rate, intr_rate, fork_rate;

    /* power settings (cpufreq / intel_pstate / ACPI platform profile); "" = n/a */
    char driver[24], governor[24], epp[40], pstate[16], profile[32];
    int turbo;                    /* 1 on, 0 off, -1 unknown */
    double lim_min, lim_max;      /* scaling limits in MHz, -1 = unknown */
    double hw_min, hw_max;        /* what the hardware can do, MHz */
    /* idle states (cpuidle), averaged over all CPUs */
    int nidle;
    cstate_t idle[MAX_CSTATES];
    double wakeups; /* idle-state entries per second, about the number of wake-ups */

    /* private */
    cpu_jiffies_t prev_all, prev[MAX_CPUS];
    unsigned long long prev_ctxt, prev_intr, prev_forks;
    int primed, freq_src;
    unsigned long long idle_time[MAX_CPUS][MAX_CSTATES], idle_use[MAX_CPUS][MAX_CSTATES];
    double idle_t;
    int idle_primed;
} cpu_t;

void cpu_init(cpu_t *c);
void cpu_sample(cpu_t *c, double dt);
void cpu_power_sample(cpu_t *c); /* governor, turbo...: slow-changing, every few seconds */

/* --------------------------------------------------------------- Memory */
#define MAX_SWAPS 8

/* a swap device from /proc/swaps; zram is compressed RAM */
typedef struct {
    char name[48];                  /* "zram0", "nvme0n1p3", "/swapfile" */
    int zram, file;                 /* compressed RAM; a swap file (else a partition) */
    unsigned long long size, used;  /* bytes */
    int prio;
    unsigned long long orig, compr, ram; /* zram: what's stored, compressed to, and the RAM it takes, bytes */
    char algo[16];                  /* zram: the compressor */
} swapdev_t;

typedef struct {
    unsigned long long total, free, avail, buffers, cached, sreclaim, shmem;
    unsigned long long swap_total, swap_free, dirty, writeback;
    unsigned long long used, buffcache, swap_used; /* derived */
    double swapin_bps, swapout_bps, majflt_rate;
    unsigned long long oom_kill; /* processes killed for lack of memory since boot */
    int oom_ok;                  /* the kernel reports oom_kill */

    unsigned long long p_swpin, p_swpout, p_majflt;
    int primed;
    int nswap, nzram;
    swapdev_t swap[MAX_SWAPS];
    int zswap;                      /* zswap (a compressed cache in front of swap): 1 on, 0 off, -1 not built in */
    char zswap_algo[16];
    unsigned long long zswap_stored, zswap_pool; /* bytes, root only (debugfs); 0 = unknown */
} mem_t;

void mem_sample(mem_t *m, double dt);

/* -------------------------------------------------------------- Storage */
#define MAX_DISKS 64
#define MAX_FS 64

typedef struct {
    char name[32], model[64], kind[8];
    unsigned long long size, rd_total, wr_total; /* bytes */
    double rd_bps, wr_bps, rd_iops, wr_iops, util;
    double rd_lat, wr_lat; /* average time per read / write in ms over the last sample */
    unsigned inflight;     /* requests in progress right now */

    unsigned long long p_rsec, p_wsec, p_rio, p_wio, p_ticks, p_rms, p_wms;
    int primed, seen;
} disk_t;

typedef struct {
    char mount[128], dev[96], type[16];
    unsigned long long total, used, avail;
    unsigned long long files, ffree; /* inodes (file slots); files 0 = the file system has no fixed number */
    int readonly;
} fs_t;

typedef struct {
    int n;
    disk_t d[MAX_DISKS];
    int nfs;
    fs_t fs[MAX_FS];
    double rd_bps, wr_bps;
} disks_t;

void disks_sample(disks_t *ds, double dt);
void fs_sample(disks_t *ds);

/* -------------------------------------------------------------- Network */
#define MAX_NETIF 64

/* Wi-Fi details from nl80211 */
typedef struct {
    int ok;                    /* connected: these are known */
    int sta;
    int freq, center, width;   /* MHz: the channel's frequency, its centre, its width */
    int channel, gen;          /* gen: Wi-Fi 4 (n), 5 (ac), 6 (ax), 7 (be); 0 older or unknown */
    int signal;                /* dBm, 0 = unknown */
    double tx_mbps, rx_mbps;   /* link speed each way, -1 = unknown */
    unsigned long long tx_pkts, tx_retries, tx_failed;
    unsigned connected_s, beacon_loss;
    double retry_pct;          /* frames sent again, of all sent, lately; -1 = unknown */
    double win_t;              /* ... counted from here (monotonic) */
    unsigned long long win_pkts, win_retries;
    int band_told;             /* the frequency the last band event was about */
    double band_since;         /* on another band since (monotonic), 0 = no */
} wifi_t;

typedef struct {
    char name[32], state[16], addr[48];
    int up, is_virtual, wireless, speed; /* speed in Mb/s, -1 = unknown */
    unsigned long long rx_total, tx_total;
    double rx_bps, tx_bps;
    unsigned long long errors, drops; /* since boot, both directions */
    double err_rate, drop_rate;       /* per second */
    double signal_dbm;                /* Wi-Fi signal, 0 = n/a */
    int quality;                      /* Wi-Fi link quality %, -1 = n/a */
    long long link_downs;             /* times the link (cable, Wi-Fi) went down since boot, -1 = n/a */
    long long downs_seen;             /* health: the count already reported */
    double down_t;                    /* health: when the link went down (wall), 0 = it's up */
    int primed, seen;
    wifi_t wifi; /* wireless interfaces */
} netif_t;

/* network quality: the router and one internet address pinged every 2 s
 * (netcheck.c, off unless switched on) */
enum { NQ_ROUTER, NQ_NET, NQ_COUNT };
enum { NQ_FINE, NQ_LOSS, NQ_SLOW };   /* trouble right now */
typedef struct {
    char addr[48];                    /* what is pinged, "" = nothing (no router) */
    int noroute;                      /* the network says it can't be reached */
    double rtt_ms;                    /* the latest answer, -1 = lost, or none yet */
    double avg_ms, min_ms, max_ms, jitter_ms; /* the last minute's answers, -1 = none */
    double loss_pct;                  /* the last minute, -1 = nothing sent yet */
    double normal_ms;                 /* its usual delay, while all was well, -1 = unknown */
    int trouble;                      /* NQ_* now (after a few pings in a row) */
    int episode;                      /* counts troubles: a new number when one starts */
    double since, until;              /* the latest trouble started / ended (wall); until 0 = still on */
    double worst_loss, worst_ms;      /* the worst of this trouble */
} nqt_t;
typedef struct {
    int on;                           /* the check runs */
    int err;                          /* it can't: errno of the ping socket (EACCES: no permission) */
    char iface[32];                   /* the interface the router is on */
    nqt_t t[NQ_COUNT];
} netq_t;
void netq_set(int on, const char *host); /* start or stop it (the setting); host "" = 1.1.1.1 */
void netq_read(netq_t *out);             /* a copy of the figures */

typedef struct {
    int n;
    netif_t i[MAX_NETIF];
    double rx_bps, tx_bps;
    netq_t q; /* the network check's figures, copied every sample */
} net_t;

void net_sample(net_t *n, double dt);
void wifi_sample(netif_t *f, double mono);                         /* in net_sample, for a wireless interface */
int wifi_band_moved(netif_t *f, double mono, char *out, size_t n); /* 1: a band event to tell */
int wifi_channel(int mhz);
const char *wifi_band(int mhz); /* "2.4 GHz", "5 GHz", "6 GHz", "" */
void net_addrs(net_t *n);

/* ------------------------------------------------------------ Processes */
typedef struct {
    pid_t pid, ppid;
    uid_t uid;
    char state, comm[24];
    char *cmdline; /* loaded lazily by proc_cmdline(), owned by the record */
    int prio, nice, nthreads, kthread, io_ok;
    unsigned long long ticks, start, vsize, rss, io_r, io_w;
    double cpu, mem, rd_bps, wr_bps;
    int nfds;                   /* open files, -1 = unknown (other user's process) */
    double gpu;                 /* GPU busy %, -1 = not using a GPU / unknown */
    unsigned long long gpu_mem; /* GPU memory in bytes */

    /* private: open-file and GPU client tracking */
    double fd_t, drm_t;
    int drm_nfds, ndrm;
    int drm_fd[6];
    unsigned long long gpu_ns[8]; /* busy time per GPU engine (procs_t.geng) */
    int gpu_primed;
} proc_t;

typedef struct {
    proc_t *p, *prev; /* both sorted by pid */
    int n, cap, nprev, prevcap;
    int threads, running, sleeping, diskwait, zombie, stopped, io_denied;
    long hz, pagesize;
    void *dir; /* DIR* on /proc, kept open between samples */
    int gpu_any;       /* some process reported GPU use (DRM fdinfo or NVML) */
    int drm;           /* the machine has DRM (GPU) devices */
    int ngeng;         /* GPU engine names seen in fdinfo ("render", "gfx"...) */
    char geng[8][24];
    double gcap[8];    /* engine capacity (parallel units) */
    pid_t slow_pid;    /* the process that took longest to read this time, and how long */
    double slow_s;
} procs_t;

void procs_init(procs_t *ps);
void procs_sample(procs_t *ps, double dt, unsigned long long memtotal);
void procs_free(procs_t *ps);
const char *proc_cmdline(proc_t *p);
int proc_send_signal(pid_t pid, unsigned long long start, int sig); /* 0 or errno */
int proc_renice(pid_t pid, unsigned long long start, int nice);    /* every thread; 0 or errno */
const char *uid_name(uid_t uid);

/* ------------------------------------------------------ Sensors (hwmon) */
enum { SENS_TEMP, SENS_FAN, SENS_POWER, SENS_VOLT };
#define MAX_SENSORS 256

typedef struct {
    int type, chip_idx, ok;
    char chip[32], label[48], path[PATHBUF];
    double value, crit; /* °C / RPM / W / V ; crit -1 = none */
    double peak, fmax;  /* highest value seen; fans: fanN_max if exposed */
    double lo, hi;      /* voltages: the chip's min / max limits, 0 = none */
    double hot_t;       /* health: when a "too hot" event was last raised */
    int fan_seen;       /* health: this fan has been seen spinning */
    int fan_flagged;    /* health: "stopped while hot" was reported */
    double fan_zero_t;  /* health: since when it reads 0 while hot */
    /* your own name and limits ("sensor = ..." lines in the config file) */
    char id[96];        /* "chip/label": stays the same across boots */
    char name[48];      /* "" = the label */
    double ulo, uhi;    /* your limits in the sensor's unit, 0 = none (fans: ulo = minimum RPM) */
    int hidden;         /* left out of the screen and the logs */
    double lim_t;       /* health: since when it is past your limit, 0 = it isn't */
    int lim_flagged;    /* health: reported */
    /* a reading that never moves while the others do: a placeholder the
     * firmware fills in (ACPI's 27.8 °C), or an input with nothing on it */
    double vmin, vmax, t_first;
    int still;
} sensor_t;

typedef struct {
    int n;
    sensor_t s[MAX_SENSORS];
    int cpu_idx; /* sensor used as "CPU temperature", -1 = none */
    char slow[32]; /* the chip that took longest to read this time, and how long */
    double slow_s;
} sensors_t;

/* your settings for one sensor, kept even while it isn't there (a USB
 * device, a graphics card that's out) */
#define MAX_SRULES 128
typedef struct {
    char id[96];   /* sensor_t.id */
    char name[48]; /* "" = keep the label */
    double lo, hi; /* 0 = none */
    int hide;
} srule_t;
void sensors_apply(sensors_t *S, const srule_t *r, int n);
const char *sensor_label(const sensor_t *t);
int sensor_plausible(const sensor_t *t); /* 0: a temperature nothing reaches (an input with nothing on it) */ /* your name, or the chip's label */
int sensor_bad(const sensor_t *t);           /* past your limits (voltages: or the chip's) */

void sensors_init(sensors_t *s);
void sensors_sample(sensors_t *s);
double sensors_cpu_temp(const sensors_t *s);
double sensors_core_temp(const sensors_t *s, int pkg, int core); /* coretemp "Core N", -1 */
void sensors_chip_name(const char *chip, char *out, size_t n); /* "nvme#2" -> "NVMe 2" */
void sensor_short_name(const sensors_t *S, int i, char *out, size_t n); /* "ACPI 2", "BAT1", "Core 3" */

/* ------------------------------------------------------------------ GPU */
#define MAX_GPUS 8
#define MAX_ENG 5

typedef struct {
    char name[8];
    int fd;
    unsigned long long prev;
    double pct;
} gpu_engine_t;

#define MAX_SCREENS 8

/* a screen on one of a GPU's ports (DRM connectors; reading them doesn't wake the GPU) */
typedef struct {
    char conn[24];    /* "eDP-1", "HDMI-A-1", "DP-2" */
    char monitor[24]; /* the name in its EDID, "DELL U2720Q"; "" unknown */
    int w, h;         /* its preferred mode */
    double inch;      /* diagonal, from the EDID's size; 0 = unknown */
    int builtin;      /* the laptop's own panel (eDP, LVDS, DSI) */
    int on;           /* in use (connected, and something draws on it) */
} screen_t;

typedef struct {
    char name[96], vendor[8], driver[16], card[16], pci[24], note[80];
    int integrated;
    int nscr;                   /* screens connected to its ports */
    screen_t scr[MAX_SCREENS];
    double scr_t;               /* read at (monotonic): every 5 s */
    char scr_told[128];         /* health: the screens already told about ("HDMI-A-1,DP-1,") */
    int scr_primed;
    double util;                /* busiest engine %, -1 = unknown */
    double mem_used, mem_total; /* bytes, -1 = unknown            */
    double temp_c, power_w, power_cap_w, clk_mhz, clk_max_mhz, mem_clk_mhz, fan_pct, fan_rpm;
    int neng;
    gpu_engine_t eng[MAX_ENG]; /* Intel i915 per-engine busy (perf PMU) */
    double eng_t;
    int util_est;              /* util is an estimate (Intel: from RC6 idle time) */
    unsigned long long rc6_prev;
    double rc6_t;

    /* NVIDIA (NVML): sleeping, limits, why it runs slower than it could */
    int nv_state;          /* NV_* below; NV_NONE for other GPUs */
    double temp_lim;       /* slows itself down from here (max operating temperature), -1 = unknown */
    double temp_slow, temp_shut; /* hardware slowdown / shutdown temperatures, -1 = unknown */
    int reasons_ok;        /* the driver says why the clock is held back */
    unsigned long long reasons; /* NVML clock event reasons now */
    int slow_now;          /* held back now: GS_HEAT | GS_POWER */
    double slow_since;     /* monotonic: held back without a break since, 0 = not now */
    double busy_s, heat_s, pwr_s, held_s; /* since start: seconds busy, and held back by heat / power / either while busy */
    double slow_ev_t;      /* when the last "slowing down" event was raised */
    double enc_pct, dec_pct;          /* video encoder / decoder busy, -1 = unknown (NVIDIA, while awake) */
    int pcie_gen, pcie_width;         /* the PCIe link now, 0 = unknown (NVIDIA, while awake) */
    int pcie_gen_max, pcie_width_max; /* the best this card and its slot can do, 0 = unknown */

    /* private */
    char dev[PATHBUF], hwmon[PATHBUF], drm[PATHBUF];
    unsigned long long p_energy;
    double p_energy_t;
    void *nvml; /* nvmlDevice_t while the GPU is read through NVML */
    unsigned long long nv_ts; /* NVML per-process samples already seen */
    char pm_path[PATHBUF];    /* its PCI device's power/runtime_status: asleep or not, without waking it */
    int pm_auto;              /* the kernel may power it off when idle (power/control = auto, driver agrees) */
    double nv_idle_t;         /* monotonic: idle (0% load) without a break since, 0 = busy */
    double nv_wait_t;         /* NV_IDLE: when NVML let go of it */
    double last_power;        /* NV_IDLE: the last figure, for the totals */
    int nv_fails;             /* let go of it, and it stayed on anyway (something else uses it) */
} gpu_t;

enum { NV_NONE, NV_ON, NV_IDLE, NV_ASLEEP };
enum { GS_HEAT = 1, GS_POWER = 2 };

typedef struct {
    int n;
    gpu_t g[MAX_GPUS];
    double nv_keep_until; /* NVML stays attached until then (the GPU didn't sleep when let go) */
    double nv_retry_t;    /* NVML init failed: try again from then */
    char slow[96];        /* the GPU that took longest to read this time */
    double slow_s;
} gpus_t;

void gpus_init(gpus_t *g);
void gpus_sample(gpus_t *g, double dt);
const char *gpu_screen_holds(const gpu_t *g, char *out, size_t n); /* a screen keeping a dGPU awake: "HDMI-A-1 (DELL U2720Q)" */
void gpus_close(gpus_t *g);
void gpus_proc_sample(gpus_t *g, procs_t *ps); /* NVIDIA: per-process GPU use through NVML */
int pci_lookup_name(unsigned vendor, unsigned device, char *out, size_t n); /* from pci.ids */

/* ---------------------------------------------------------------- Power */
#define MAX_RAPL 16

typedef struct {
    char name[32], path[PATHBUF];
    int depth, readable, primed;
    unsigned long long max_range, prev;
    double prev_t; /* when prev was read: watts come from the time between two reads */
    double watts;
} rapl_t;

typedef struct {
    char name[16], status[24], path[PATHBUF];
    double watts, capacity, time_left; /* -1 = unknown; time_left in s */
    double health;                     /* full charge now vs. new, %, -1 = unknown */
    double full_wh, design_wh;         /* what a full charge holds now / when new, -1 = unknown */
    double energy_wh, energy_full_wh;  /* the charge in it now / when full, Wh, -1 = unknown */
    long cycles;                       /* charge cycles, -1 = unknown */
    /* a charge limit (laptops that can stop charging early to slow wear) */
    int lim_ok;              /* the battery has the setting */
    int lim_end, lim_start;  /* stops at / starts again below, %; -1 = not set */
    double lim_t;            /* when it was last read (monotonic) */
} battery_t;

/* how much the CPU package may draw: readable without root. The same limits
 * can be set twice, in the CPU's registers (MSR) and by the firmware through
 * MMIO; the lower of the two applies. */
typedef struct {
    int ok;                          /* the package has a sustained limit */
    double long_w, long_s;           /* sustained (PL1) and the window it is averaged over; -1 = none */
    double short_w;                  /* short boosts (PL2), -1 = none */
    int long_off;                    /* the sustained limit is switched off */
    double fw_long_w, fw_short_w;    /* the firmware's copy (MMIO), -1 = none */
    double eff_long_w, eff_short_w;  /* what applies: the lower of the two */
    double t;                        /* when they were read (monotonic) */
} cpulim_t;

typedef struct {
    int nrapl;
    rapl_t z[MAX_RAPL];
    int rapl_denied; /* zones exist but energy_uj is root-only */
    int nbat;
    battery_t bat[4];
    char ac_path[PATHBUF];
    char usbc_path[4][PATHBUF]; /* no "Mains" supply: the USB-C ports that charge (type USB) */
    int nusbc;
    int ac_online; /* -1 = unknown */
    double pkg_w, core_w, uncore_w, dram_w, psys_w; /* -1 = unknown */
    /* the "psys" (whole platform) domain is left unconnected by some laptop
     * firmware: it then reads far less than the CPU package inside it */
    int psys_low, psys_bad;
    cpulim_t lim;  /* the CPU's power limits */
    char slow[40]; /* the part that took longest to read this time, for slow-reading events */
    double slow_s;
} power_t;

void power_init(power_t *p);
void power_sample(power_t *p, double dt);

/* --------------------------------------------------------------- Health */
#define MAX_PKGS 8
#define MAX_PCIDEV 64
#define MAX_NVME 8
#define MAX_EVENTS 200

enum { EV_INFO, EV_WARN, EV_ERR };

/* Something worth knowing about: a kernel error message, throttling,
 * an out-of-memory kill, a new PCIe error... */
typedef struct {
    double t;          /* wall clock, seconds since the epoch */
    unsigned long seq; /* increasing, to find events newer than a point */
    int level;         /* EV_* */
    char cat[10];      /* "thermal", "memory", "disk", "gpu", "pcie", "crash", "kernel"... */
    char text[200];
    int rep;           /* the same line came this many times in a row ("... (3 times)") */
    double t_rep;      /* when it came last */
} event_t;

/* pressure stall information: share of time tasks waited for a resource */
typedef struct {
    int ok, has_full, primed;
    double some, full; /* % of the last sample */
    double avg10, avg60, avg300, favg10, favg60, favg300; /* kernel's averages of "some" / "full" */
    unsigned long long p_some, p_full;
} psi_t;

typedef struct {
    int pkg, cpu;                 /* package id, and a CPU in it to read from */
    unsigned long long count, ms; /* package throttling since boot: times, total ms */
    unsigned long long p_ms, p_count;
    double pct;                   /* % of the last sample spent throttled */
} thr_pkg_t;

typedef struct {
    char dev[16], name[80];
    unsigned long long cor, nonfatal, fatal; /* AER error counts since boot */
} pcidev_t;

typedef struct {
    char name[16], model[48];
    int state;    /* 1 read, 0 not yet, -1 needs root, -2 not supported */
    unsigned crit; /* critical warning bits (0 = fine) */
    double temp_c;
    int spare, spare_thr, used; /* % */
    unsigned long long hours, cycles, unsafe, media_err, err_log, warn_min, crit_min;
    double read_b, written_b;
    /* slowing itself down from heat, counted by the drive since new: [0] a little
     * (thermal management temperature 1), [1] hard (temperature 2) */
    unsigned long long heat_n[2], heat_s[2]; /* times, seconds */
    int heat_open, heat_hard;                /* slowing down now (or lately); it got to the hard kind */
    double heat_from, heat_t, heat_max_c;    /* wall clock: first and last time seen; hottest meanwhile */
    unsigned long long heat_n0[2], heat_s0[2]; /* the counters when it began */
} nvme_t;

#define MAX_SATA 8

/* a SATA SSD or hard disk, from its SMART attributes */
typedef struct {
    char name[16], model[48];
    int state;        /* 1 read, 0 not yet, -1 needs root, -2 no SMART data, -3 asleep, not woken */
    int rotational;   /* a hard disk */
    int asleep;       /* spun down at the last check (the figures are from before) */
    int failing;      /* an attribute is at or below the maker's limit */
    char fail_attr[28];
    int temp_c, wear; /* wear: % of the rated life used (SSDs), -1 = unknown */
    long long hours, cycles, realloc, pending, uncorrect, crc, unsafe; /* -1 = unknown */
} sata_t;

/* the batteries at one moment, for what they lost while the computer slept */
typedef struct {
    int ok;        /* there is a battery and it reported its charge */
    int charger;   /* plugged in (or charging / full) */
    double pct;    /* charge, % */
    double wh;     /* charge, Wh, -1 = unknown */
} batsnap_t;

typedef struct {
    psi_t psi[3]; /* cpu, memory, io */

    int thr_ok;   /* the CPU reports thermal throttling counters */
    int nthr;
    thr_pkg_t thr[MAX_PKGS];
    unsigned long long thr_count, core_count; /* since boot: package / core throttling events */
    double thr_ms, thr_pct; /* total package throttled time since boot; worst % last sample */
    double thr_last;        /* wall clock of the last throttling seen, 0 = none since start */
    unsigned long long thr_start; /* thr_count + core_count when hsm started */

    unsigned long long oom_start; /* oom_kill when hsm started */
    double oom_last;

    int aer_ok, npci;
    pcidev_t pci[MAX_PCIDEV];
    unsigned long long pci_cor, pci_nonfatal, pci_fatal;
    double pci_last;

    int nnvme;
    nvme_t nvme[MAX_NVME];
    int nsata;
    sata_t sata[MAX_SATA];

    unsigned long long files, files_max; /* open file handles, whole system */

    int kmsg;                  /* 1 reading the kernel log, -1 needs root, 0 not available */
    unsigned kmsg_err, kmsg_warn; /* relevant messages since boot */
    double kmsg_last;          /* wall clock of the newest error, 0 = none */

    int nev, ev_head;
    unsigned long ev_seq;
    event_t ev[MAX_EVENTS];

    double hot_last; /* a temperature reached its critical level */
    double slow_last, lowmem_last; /* a disk was very slow / memory ran low (alert limits) */
    double full_last;              /* a file system passed the "almost full" alert limit */

    /* freezes: the whole system stood still (watchdog thread); suspends */
    int freezes;
    double freeze_max, freeze_last, freeze_t; /* longest, latest (s), when it was noticed (wall) */
    double frozen_total;                      /* seconds frozen since start */
    double asleep_total, resume_t;            /* time suspended since start, when it last woke (wall) */
    int suspends;
    int link_drops;                           /* network links lost since start */
    int fan_stops;                            /* fans that stopped while hot */
    double fan_last, link_last, ro_last;      /* a fan is stopped / a link dropped / a disk went read-only */
    double ulim_last;                         /* a sensor is past the limit you gave it */

    /* the CPU held back by its power limit rather than by heat (needs the
     * package's power, so root) */
    int pl_known;                 /* the package's power and its limits are known */
    int pl_now;                   /* PL_LONG at its sustained limit, PL_SHORT at its boost limit, 0 not */
    double pl_held_s, pl_busy_s;  /* seconds held back / seconds busy since start */

    char slow[40];  /* the check that took longest this time, and how long */
    double slow_s;

    /* private */
    int kmsg_fd, kmsg_file, primed, thr_tried;
    long kmsg_off;
    unsigned long long oom_prev;
    unsigned long long core_ms[MAX_CPUS], core_cnt[MAX_CPUS];
    double next_slow, next_nvme, next_sata, thr_ev_t;
    char governor[24];
    int susp_ok;                   /* the kernel counts failed suspends */
    unsigned long long susp_fail;
    double pl_since, pl_last, pl_ev_t; /* the stretch began / last at the limit (mono) / last event (wall) */
    int pl_kind, pl_told;
    int nq_seen[NQ_COUNT], nq_open[NQ_COUNT]; /* network check: troubles told about / still going */
    batsnap_t bat_prev;            /* the batteries at the previous sample */
    struct {                       /* the "woke up" event waits a few seconds for the battery to settle */
        int on;
        double dur, wall, due;     /* how long it slept, when it went to sleep (wall), when to tell */
        batsnap_t pre;             /* the batteries just before */
    } wake;
} health_t;

void health_init(health_t *h);
void health_event(health_t *h, int level, const char *cat, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
const event_t *health_event_at(const health_t *h, int i); /* 0 = newest, NULL past the end */
/* what an event means, in plain words, for the ones that need it; 0 = none */
int event_explain(const char *cat, const char *text, char *out, size_t n);
/* active problems for the header: throttling now, OOM kill just now... "" = none */
int health_alert(const health_t *h, char *out, size_t n);
/* the busiest process right now, for event texts: " while firefox used 180% CPU" ("" = none) */
enum { BLAME_CPU, BLAME_MEM, BLAME_IO, BLAME_GPU };
enum { PL_LONG = 1, PL_SHORT = 2 }; /* which power limit holds the CPU back */
void health_blame(const procs_t *ps, int what, char *out, size_t n);

/* freeze.c: a watchdog thread that notices when the whole system stops */
enum { FZ_STALL, FZ_SUSPEND };
typedef struct {
    double wall, dur; /* when it started, how long */
    int kind;
} fz_t;
void freeze_start(void);
int freeze_take(fz_t *out); /* 1 = got one */

/* leak.c: processes whose memory keeps growing (leak_sample is with sys_t below) */
#define LEAK_MIN 120 /* minutes of memory history kept per process */
/* the memory history of one process, oldest first (MB a minute); returns
 * how many; *growing = it was flagged, *rate = MB per hour lately */
int leak_history(pid_t pid, unsigned long long start, float *out, int max, int *growing, double *rate);
int leak_growing(pid_t pid, unsigned long long start);
#define RUNAWAY_MIN 15 /* minutes at a whole core before a process is called out */
int runaway_minutes(pid_t pid, unsigned long long start, int *flagged); /* minutes in a row busy */
int runaway_count(void);       /* called out and still busy */

/* ata.c: SATA drives */
void ata_find(health_t *h);
void ata_sample(health_t *h);

/* selftest.c: a drive's own tests and error log */
#define MAX_STEST 21
typedef struct {
    int type;          /* 1 short, 2 extended, 3 other */
    int failed, stopped;
    char what[48];     /* "completed without error", "FAILED: read element"... */
    long long hours;   /* the drive's power-on hours when it ran */
    long long lba;     /* first bad sector, -1 = none */
} stest_t;

typedef struct {
    char name[16], model[48];
    int nvme;
    int state;               /* 1 read, 0 not yet, -1 needs root, -2 not supported */
    int can_test;
    int running, progress;   /* a test going on now: 1 short, 2 extended; % done (-1 unknown) */
    int short_min, ext_min;  /* how long the tests take, minutes, -1 = unknown */
    int ntest;
    stest_t test[MAX_STEST]; /* newest first */
    long long hours;         /* power-on hours now */
    long long errors;        /* errors the drive logged in its life, -1 unknown */
    char err_text[96];       /* the latest one */
    int err_bad;             /* ... was a real problem (media error), not a refused command */
    long long err_hours;     /* when (power-on hours), -1 unknown */
    double read_t;
    int follow;              /* hsm started a test: report when it ends */
    double follow_t;
    int sim_type;            /* test hook: a pretend test */
    double sim_until;
} drive_t;

int drives_sync(const health_t *h); /* the drives health found; returns how many */
drive_t *drive_at(int i);
int drive_read(drive_t *d);          /* self-test log, status, error log; 0 = read */
int drive_start(drive_t *d, int type); /* 1 short, 2 extended, 0 stop it; 0 or errno */
void drives_tick(health_t *h);       /* events for the tests hsm started */
int ata_smart(int fd, unsigned char feature, unsigned char lba_low, unsigned char *buf); /* 0 or errno */
void sata_summary(const sata_t *s, char *out, size_t n, int *level, const char *sep, const char *deg);

/* ------------------------------------------------- Services (cgroup v2) */
enum { SVC_SERVICE, SVC_SCOPE, SVC_CONTAINER, SVC_SESSION, SVC_APP, SVC_OTHER };

typedef struct {
    char path[256];  /* relative to the cgroup root: "system.slice/nginx.service" */
    char name[80];   /* shown: "nginx", "docker 3f2a1b2c3d4e" */
    int kind;        /* SVC_* */
    uid_t uid;       /* owner for user units, 0 = system */
    double cpu;      /* % of one CPU, like top */
    long long mem;   /* bytes, -1 = n/a */
    double rd_bps, wr_bps;
    long tasks;      /* -1 = n/a */
    unsigned long long oom; /* processes the OOM killer took from it */

    unsigned long long p_usage, p_rd, p_wr;
    int primed, seen;
} svc_t;

typedef struct {
    int n, cap, state; /* state: 1 ok, 0 not looked yet, -1 no cgroup v2 */
    svc_t *s;
    char root[PATHBUF];
    double t, next_scan, want_until;
} svcs_t;

void svcs_sample(svcs_t *v); /* only does work while want_until is in the future */
void svcs_free(svcs_t *v);
const char *svc_kind_name(int kind);

/* --------------------------------------------------------- Whole system */
/* the machine, as its firmware describes it (/sys/class/dmi/id, readable by
 * everyone; serial numbers are root-only and left out) */
typedef struct {
    int ok;
    char vendor[64], model[96], type[64]; /* type: the maker's code when the model is a name (Lenovo) */
    char board[96];
    char bios_vendor[64], bios_version[64];
    int bios_date;                        /* YYYYMMDD, 0 = unknown */
    int chassis;                          /* SMBIOS chassis type: 3 desktop, 10 notebook, 35 mini PC...; 0 = unknown */
} machine_t;
int machine_desktop(const machine_t *m); /* a desktop's chassis (or a tower, mini PC, all-in-one) */

/* ------------------------------------------------------------------ USB */
#define MAX_USB 40

/* a USB device: what it is, the speed it runs at and where it's plugged in */
typedef struct {
    char id[20];       /* where: "1-2" (bus 1, port 2), "3-1.4" (port 4 of a hub in port 1) */
    char name[64];     /* "SanDisk Ultra", "Logitech USB Receiver", or "0781:5581" */
    char kind[16];     /* "storage", "keyboard", "mouse", "camera", "audio", "hub", "Bluetooth"... */
    char blk[16];      /* the disk it gives, "sdb" (storage) */
    unsigned vid, pid;
    int devnum;        /* a new one each time it connects */
    double mbps;       /* the speed it runs at: 1.5, 12, 480, 5000, 10000, 20000 */
    int can;           /* the most it can do, Mb/s (its BOS descriptor), 0: unknown */
    int port3;         /* the port it's in can do USB 3: 1 yes, 0 USB 2 only, -1 unknown */
    int where;         /* 1 an outside socket, 2 built in, 3 behind a hub, 0 unknown */
    int depth;         /* hubs on the way */
    int slow;          /* it can do USB 3 but runs at USB 2 speed */
    int ma;            /* the power it asks for, mA */
    double seen;       /* wall clock: when it came (0: there when hsm started) */
    int told;          /* its "connected" event is out (or it needs none) */
} usbdev_t;

typedef struct {
    int ok; /* the USB tree can be read */
    int n;
    usbdev_t d[MAX_USB];
    unsigned long long sig; /* the names and device numbers, to notice a change cheaply */
    int primed, nslow;
    double next_blk;
    int ngone;
    struct {
        usbdev_t d;
        double t;
    } gone[8]; /* just disconnected: told after 3 s, unless it comes back meanwhile */
} usb_t;

typedef struct {
    char host[64], kernel[64], os[96], arch[24];
    machine_t mach;
    double t_last, dt;
    unsigned long samples;

    cpu_t cpu;
    mem_t mem;
    disks_t disks;
    net_t net;
    procs_t procs;
    sensors_t sens;
    gpus_t gpus;
    power_t power;
    health_t health;
    svcs_t svc;
    usb_t usb;

    double total_w; /* best whole-machine power estimate, -1 = unknown (never a partial sum) */
    const char *total_w_src;
    const char *total_why; /* why it's unknown: "needs root" (the CPU part), "" */
    int pw_partial;        /* the power history holds only the GPUs (no total known) */
    double part_w;         /* ... and what they draw */

    hist_t h_cpu, h_mem, h_gpu[MAX_GPUS], h_dr, h_dw, h_rx, h_tx, h_pw, h_temp;
    double next_slow;

    /* a reading that took a second or more: reported once the next sample
     * shows the whole system didn't stand still meanwhile (that's a freeze) */
    char slow_what[96];
    double slow_dur;
    int slow_frz, slow_susp;
    struct {
        char what[96];
        double t;
    } slow_seen[8]; /* each one at most every 10 minutes */
} sys_t;

void sys_init(sys_t *s);
void sys_sample(sys_t *s);
void sys_free(sys_t *s);
void health_sample(sys_t *s); /* after the other collectors: needs CPU, memory and sensors */
void usb_sample(sys_t *s);    /* after health_sample: its events, quiet after a resume */
const char *usb_speed(double mbps, char *out, size_t n); /* "USB 2 480M", "USB 3 10G" */
void leak_sample(sys_t *s);   /* once a minute, from health_sample */
void runaway_sample(sys_t *s); /* once a minute, from health_sample */
void health_close(health_t *h);

#endif

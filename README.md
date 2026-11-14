# hsm — hardware status monitor

A fast, lightweight hardware and process monitor for the Linux terminal, written in C. It shows everything your machine is doing on one screen: CPU, GPU, memory, power draw, disk and network throughput, temperatures, fans, voltages, every process (as a list or a tree) with its CPU, memory, disk I/O, GPU use and open files, and every systemd service and container. A **Health** panel watches for the things that come before a crash: thermal throttling, the whole system freezing, tasks stalling for CPU, memory or disk, out-of-memory kills, fans that stop, disks filling up or turning read-only, network links dropping, processes whose memory only ever grows or that keep a core busy for a long time, PCIe bus errors, NVMe and SATA drive wear and errors, and the kernel's own error messages. You choose which panels appear, arrange them, and pick how bars and graphs look. Colours are your terminal's standard ones, so they follow your terminal's theme, and you can pick the background, the selection colour and the text colours from them.

It is also built to **catch rare failures**: a flight recorder keeps the last 10 minutes of every reading and saves them when something goes wrong, the next start tells you if the computer went down while hsm was running (with the kernel's crash report when the firmware kept one), desktop alerts warn you when a limit is crossed, and a log viewer graphs saved logs right in the terminal. And it **remembers**: every event is kept across runs and reboots in a searchable list, and one line per day of history shows slow changes, such as a machine that runs a few degrees hotter every month.

* **Fast**: reads `/proc` and `/sys` directly and never parses other programs' output. One full sample of every subsystem takes about 0.3 ms. (The only program it ever starts is `notify-send`, for desktop alerts.)
* **Small**: about 750 KB binary, about 5 MB RAM, a fraction of a percent of one core at a 1 s refresh (2 MB and almost nothing as `--watch`), flight recorder included.
* **Health**: throttling, freezes, pressure stalls, OOM kills, stopped fans, full or read-only disks, dropped network links, growing and runaway processes, PCIe errors, NVMe and SATA drive health (an SSD slowing itself down from heat too), USB 3 drives held to USB 2 speed and kernel errors in one panel, with an alert in the header while something is wrong. Each event names the busiest process at that moment.
* **Catches failures**: the flight recorder, freeze detection, crash detection across reboots and alerts (see [Catching failures](#catching-failures)).
* **Long memory**: every event kept across runs (view `7`), one line per day of history with trends and what ran that day (view `8`), a cooling check that compares with the previous one, and the drives' own self-tests and error logs.
* **Logging**: record temperatures, load, clocks, power, fans, disk and network to CSV files in daily folders, follow single processes, delete old logs automatically, and graph any of it in the built-in log viewer, which opens on a summary of what the log shows.
* **Laptop-friendly**: a sleeping NVIDIA GPU is left asleep, and hsm lets it power off when it's idle (asking it for its load every second would keep it on at 5-10 W); a screen plugged into a port wired to that GPU is named as the reason it can't sleep; why the GPU is slowing down (heat or its power limit), and the CPU too (heat, or the power limit it shares with the GPU); what the battery lost while the laptop slept; battery wear and charge limit.
* **Wi-Fi and USB**: the Wi-Fi's band (2.4, 5 or 6 GHz), channel and link speed next to its signal; every USB device with its speed and the port it's in, a USB 3 drive running at USB 2 speed called out, and plugging in and out kept as events.
* **Network check** (off until you switch it on): the delay and lost pings to your router and to the internet, which tells a Wi-Fi problem from one further away and explains lag spikes in games.
* **Easy to use**: mouse support (click tabs, rows and column headings, scroll with the wheel), everything about a process with `Enter`, process tree, priority (nice) and signals from the process list, `--json` for scripts, `--serve` for Prometheus and Grafana.
* **Customizable**: show, hide and rearrange panels; choose bar style, graph style, colour mode, selection and title colours, and the background; give sensors your own names and limits. Settings are saved automatically.
* **Runs on most PCs**: desktops, laptops, mini PCs, servers and virtual machines; Intel, AMD and ARM CPUs; AMD, Intel and NVIDIA graphics, and the GPUs of ARM boards such as the Raspberry Pi; 64- and 32-bit. What a machine doesn't report is left out or marked n/a, never shown as a made-up zero.
* **No hard dependencies** beyond ncurses. NVIDIA support is loaded at runtime only if the driver is installed.

## What it shows

| Panel | Details | Source |
|---|---|---|
| CPU | total, and a row per core: a bar for usage, temperature or clock speed (your choice, key `b`) with the other two next to it as numbers; user/sys/iowait/steal, average clock and what holds it down (*slowing: heat* or *power limit*), package temperature and power, load average, context switches, interrupts and forks per second, history graph | `/proc/stat`, cpufreq, hwmon, RAPL |
| GPU | load, VRAM, core clock bar, memory clock, temperature, power and power limit, fan; NVIDIA: whether it's asleep, why it's slowing down (heat or its power limit), its video encoder and decoder while in use, and its PCIe link | see below |
| Memory | used/available, cache, shared, dirty, swap usage, swap-in/out rate, major page faults; zram: how full it is, how well it compresses and the RAM it takes | `/proc/meminfo`, `/proc/vmstat`, `/proc/swaps`, zram |
| Power | CPU package, cores, iGPU (uncore), DRAM, platform (psys), GPU power, battery drain and time left, charge limit, AC state, estimated total (only a complete one: see [How power is estimated](#how-power-is-estimated)) | RAPL powercap, hwmon, `power_supply` |
| Disk I/O | read/write throughput, IOPS, busy % and **time per request** per physical drive | `/proc/diskstats`, `/sys/block` |
| Network | download/upload rate per interface, link speed, IPv4 address, errors and dropped packets, Wi-Fi signal, band, channel and link speed ([Wi-Fi details](#wi-fi-details)); with the [network check](#network-check) on, the delay and lost pings to the router and the internet | `/proc/net/dev`, `/proc/net/wireless`, nl80211, pings |
| Temperatures | every temperature sensor (CPU package and each core, NVMe drives, the GPU - NVIDIA's too -, chipset, Wi-Fi...), named so you can tell them apart (*ACPI 1*, *ACPI 2*), with CPU temperature history | `/sys/class/hwmon`, NVML |
| Fans | every fan speed sensor, plus NVIDIA GPU fans; side by side when the panel is short of room, so none goes missing | `/sys/class/hwmon`, NVML |
| Filesystems | used/total for local filesystems | `/proc/self/mounts`, `statvfs` |
| Processes | PID, user, state, threads, nice, CPU %, MEM %, RSS/VIRT, **disk read/write per process**, **GPU % per process**, open files, CPU time, full command line; as a list or a **tree** (which process started which); a `!` after the CPU % of a process that has kept a whole core busy for 15 minutes, a `↑` after the memory of one that only grows | `/proc/[pid]/*`, DRM fdinfo, NVML |
| Health | thermal throttling, the CPU's power limit holding it back, freezes, pressure stalls (CPU / memory / disk), out-of-memory kills, PCIe errors, NVMe and SATA drive health (an SSD slowing itself down from heat), a USB 3 drive held to USB 2 speed, kernel errors, and a list of recent events | see [Health](#health-finding-the-cause-of-a-failure) |
| Services | CPU, memory, disk I/O and tasks per systemd service, login session, desktop app and container (a panel; the full table is view `6`) | cgroup v2 |
| Voltages | every voltage the sensor chips report (the motherboard, a laptop's battery, USB-C ports), in red when outside its limits (or yours) | `/sys/class/hwmon` |

The **Hardware** view (key `3`) lists the machine (its model, board and BIOS version and date, as the firmware describes it; a desktop by its board, an ARM board by its device tree), every sensor (an input with nothing connected to it, reading 127 °C or -62 °C, is grayed out and left out of the dashboard), GPU detail and the [screens](#screens) on each GPU's ports, RAPL power domain, drive (with totals since boot), filesystem, network interface ([Wi-Fi](#wi-fi-details) in full), swap device ([zram](#zram-and-swap) with its compression) and [USB device](#usb-devices), plus the full health details (each drive's SMART figures, freezes and suspends, how the last sessions ended when they didn't end normally), the CPU's power settings (driver, governor, energy preference, turbo, frequency limits, platform profile) and [power limits](#cpu-power-limits), the network check's figures, how long the CPUs sleep in each idle state, battery wear, charge cycles and charge limit, and readings that haven't changed since hsm started (a placeholder the firmware fills in, or an input with nothing on it). In it, `D` shows the [drives' self-tests](#drive-self-tests) and `C` runs the [cooling check](#cooling-check).

Two more views keep what happened: **Events** (key `7`), everything hsm noticed since you started using it, and **History** (key `8`), one line per day. See [Events and history](#events-and-history).

### zram and swap

When swap is compressed RAM (zram, the default on Fedora and common on Arch), the Memory panel gets a line for it: how full it is, how much smaller it keeps what's in it, the RAM it takes and the compressor (*zram 1.20G/8.00G  3.2x smaller  takes 400M  zstd*). The Hardware view lists every swap device (zram, swap files and partitions, the last two with their priority) and says when zswap, a compressed cache in front of swap, is on (with what it holds, when run as root). `--once`, `--json` and `/metrics` have the same figures.

### GPU support

| Vendor | How | Notes |
|---|---|---|
| Intel (i915) | engine busy % (3D / Video / Blit) from the i915 perf counters, the same source `intel_gpu_top` uses. Clocks from sysfs. iGPU power from RAPL "uncore". | Exact load needs root or `CAP_PERFMON`. Without it, hsm estimates load from the GPU's RC6 sleep time (shown as `Load~`). |
| AMD (amdgpu) | `gpu_busy_percent`, VRAM, plus hwmon temperature, power, clocks and fan | Works as a normal user. |
| NVIDIA (proprietary) | NVML (`libnvidia-ml.so`), loaded with `dlopen()` at startup; whether it's asleep from its PCI device in sysfs | Works as a normal user. Nothing is needed at build time. A laptop GPU that is powered off is left alone (see below). Video engines and the PCIe link too, while it's awake. |
| Others (nouveau, xe, virtual machines' GPUs...) | whatever the driver exposes through hwmon | Load isn't available for these yet. |
| ARM boards (VideoCore, Mali, Adreno...) | the GPU and the screens on its ports | No load figures from these drivers yet. |

### Screens

The Hardware view lists the screens on each GPU's ports: the port (*eDP-1*, *HDMI-A-1*, *DP-2*), the monitor's name and size from its EDID (*DELL U2720Q, 27", 3840x2160*), and a laptop's own panel as *the built-in screen*. They come from what the kernel already knows, so reading them doesn't wake a sleeping GPU.

On most laptops the HDMI port (and often a USB-C port) is wired to the NVIDIA GPU: a screen plugged in there keeps that GPU awake, at several watts, for as long as it's connected. hsm names it as the reason (*awake: the screen on HDMI-A-1 (DELL U2720Q) keeps it on* in the Hardware view, *awake for the screen on HDMI-A-1* in the GPU panel), adds an event when it starts, and keeps reading the GPU meanwhile instead of letting go of it, which couldn't let it sleep anyway. Once the screen is unplugged, it lets go as usual.

### NVIDIA GPUs on laptops

A laptop's NVIDIA GPU powers itself off when nothing uses it (runtime D3) and back on when something does, and every NVML query counts as use: a monitor that asks for its load every second keeps it on for good, at 5-10 W, which matters on battery. hsm doesn't:

* whether it's asleep comes from its PCI device in sysfs, which doesn't wake it. A GPU that is asleep when hsm starts is left alone and shows as *asleep: powered off to save power* (0 W in the power figures);
* after 10 s at 0% load, hsm lets go of NVML so the GPU can power off; when something wakes it (a game, a video), hsm picks it up again;
* if it stays on anyway, something else holds it. A [screen](#screens) on one of its ports is named, and hsm keeps reading the GPU until it's unplugged; for anything else (a program running on it) the Hardware view says so, hsm keeps reading it, and tries letting go again after 5, 10, 20, then every 40 minutes;
* desktops, and laptops with runtime power management switched off, are read as always.

`hsm --once` and `--json` don't wake a sleeping GPU either. If NVML doesn't start (most often right after a driver update, before a reboot), the GPU says why instead of showing as idle.

**Why it's slowing down.** NVIDIA's driver says why the GPU runs below its top clock. hsm shows it next to the temperature (*slowing: heat*, *slowing: power limit*), in the Health panel, in the Hardware view with the card's own temperature limits (where it starts slowing down, its hardware slowdown and shutdown temperatures), as an event when it lasts, and in two log columns. A laptop GPU at its temperature limit under heavy load is normal; spending most of a game there means the cooling can't keep up (dust, a blocked vent, a soft surface under the laptop) and costs frame rate. The *GPU slowing* alert tells you when it has gone on for minutes (see [Alerts](#alerts)).

**Video and the PCIe link.** While the GPU is awake, hsm also shows how busy its video encoder and decoder are (recording, streaming, video played back on the GPU), next to the temperature while they are in use, and the PCIe link it runs at (*PCIe 4.0 x8*). An idle card drops to a slower link to save power and speeds it up again under load; one that stays slow under load has a problem with its slot, riser or power settings. Like everything else, these are only read while the GPU is awake anyway.

## Build and install

You need a C compiler, `make` and the ncurses development headers.

```sh
# Debian / Ubuntu / Mint / Raspberry Pi OS
sudo apt install build-essential libncurses-dev pkg-config
# Fedora
sudo dnf install gcc make ncurses-devel pkgconf
# Arch / Manjaro
sudo pacman -S --needed base-devel ncurses
# openSUSE
sudo zypper install gcc make ncurses-devel pkg-config
# Alpine
sudo apk add build-base ncurses-dev linux-headers pkgconf
# Void
sudo xbps-install -S base-devel ncurses-devel pkg-config

git clone https://github.com/Yormask/linux-hardware-stats
cd linux-hardware-stats
make
./hsm                     # run from the build folder
sudo make install         # installs to /usr/local/bin/hsm
```

After installing, run `hsm` from anywhere. Use `sudo hsm` to also see power, exact Intel GPU load, the kernel log and NVMe / SATA drive health, and to signal other users' processes (see [Permissions](#permissions-what-needs-root)).

For desktop alerts, `notify-send` should be installed (package `libnotify` on Arch and Fedora, `libnotify-bin` on Debian and Ubuntu). Most desktops have it already; without it, alerts use the terminal bell.

`make debug` builds with AddressSanitizer and UBSan for development. `make uninstall` removes the installed binary.

**Upgrading from 1.9**: nothing changes in your settings. New logs get a column for each NVMe drive's time slowed down by heat and two for each Wi-Fi interface (link speed and frequency). USB devices connecting and disconnecting are now events, in the *usb* category, next to the USB errors from the kernel. The battery no longer suggests setting a charge limit.

**Upgrading from 1.8**: there are two new alerts, *GPU hotter than* (90 °C) and *GPU slowing* (on), and a network check that stays off until you switch it on (Setup, Behaviour). The History view now also keeps what ran each day, in `programs.csv` next to `history.csv`. Nothing else changes in your settings.

**Upgrading from 1.7**: nothing changes in your settings. Without root, the power total is now left empty in the Power panel, logs and history, where it used to be the GPUs alone. USB devices that fail to connect are now warnings: they no longer start an incident log or a *Problems* alert.

**Upgrading from 1.6**: views `7` (Events) and `8` (History) are new and start filling from the first run; nothing else changes in your settings. Logs and `--json` output now always use a decimal point: before, a language that writes decimals with a comma (German, French...) broke the CSV columns and the JSON.

**Upgrading from an older version** of hsm: new panels appear where they would be by default (Health below the GPU; Services and Voltages hidden). Your other settings stay as they were; the old `cpu_bars` setting becomes the new per-core display (the bar shows usage if that was on, otherwise temperature, otherwise clock speed). Logs from before 1.6 stay where they are; new ones go into daily folders next to them.

**Upgrading from a version** that was called `lhs` or `hardware-stats`: remove the old binary with `sudo rm -f /usr/local/bin/lhs /usr/local/bin/hardware-stats`.

## Usage

```
hsm [options]
  -d, --delay SEC   refresh interval in seconds (default: saved setting or 1)
  -1, --once        print one plain-text snapshot and exit (good for scripts / ssh)
  -j, --json        print one snapshot as JSON and exit (for scripts)
  -n, --top N       processes listed by --once (default 15) or --json (default all)
  --view FILE       open a saved log in the log viewer
  -a, --ascii       plain ASCII drawing for terminals without Unicode
  -h, --help        help
  -v, --version     version, copyright and license

Without the screen (runs until Ctrl+C or kill; can be combined):
  --watch           flight recorder, alerts, freeze and crash detection, events and history
  --log             log the values chosen in the Logging view (5) to a CSV file
  --log-pid PID     log one process's usage (can be given up to 16 times)
  --log-every SEC   seconds between rows (default: the saved setting)
  --log-dir DIR     where to write the files (default: ~/.local/state/hsm/logs)
  --serve [ADDR:]PORT  answer over HTTP: /metrics (Prometheus), /json; only this
                    machine unless ADDR says otherwise (0.0.0.0:9101: everyone)

  --cooling-check   a minute of full CPU load: does the cooling keep up? (exit 1: no)
```

### Keys

| Key | Action |
|---|---|
| `1` ... `8` / `Tab` | **Dashboard** / **Processes** (full table) / **Hardware** (every sensor and device) / **Setup** / **Logging** / **Services** / **Events** / **History** |
| `Enter` | everything about the selected process (see [Process details](#process-details)) |
| `s` / `F2` | open Setup |
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | move the selection, or scroll the Hardware view |
| `l` | start or stop logging sensors to a CSV file (see [Logging](#logging)) |
| `L` | start or stop logging the selected process |
| `x` / `F9` | send a signal to (kill) the selected process |
| `N` | change the priority (nice) of the selected process; `F7` / `F8` raise / lower it one step without the dialog |
| `T` / `F5` | process tree: children under the process that started them (again: back to the list) |
| `c` `m` `d` `p` `n` `t` | sort by CPU, memory, disk I/O, PID, name, CPU time. Press the same key again to reverse. |
| `g` `o` | sort by GPU use, by open files |
| `r` | reverse the sort |
| `/` | filter by process name, command line, user or PID (`Enter` keeps it, `Esc` clears it) |
| `b` | what the CPU core bars show: usage, temperature or clock speed |
| `v` (in view `5`) | saved logs and the log viewer (see [Viewing saved logs](#viewing-saved-logs)) |
| `D` `C` (in view `3`) | the drives' self-tests and error logs / the cooling check |
| `k` | show or hide kernel threads |
| `f` | selection follows the process instead of staying on the row |
| `+` `-` | slower / faster refresh (0.25 s to 10 s) |
| `Space` | pause sampling (logging pauses too) |
| `?` | help |
| `q` / `F10` | quit |

**Mouse**: click a tab to switch views, a row to select it, a column heading to sort by it (click it again to reverse), the arrows in Setup to change a value, the boxes in Setup and in the Logging view to tick them, and a key hint in the footer to press that key. Double-click a process to see everything about it, a service to see its processes, a saved log to open it. The wheel scrolls lists, and zooms the graph in the log viewer. While the mouse is on, the terminal hands clicks to hsm, so select text with `Shift` held down (most terminals); or switch the mouse off in Setup.

## Customizing the dashboard

Press `s` (or `4`) to open **Setup**. Changes apply immediately and are saved automatically.

**Dashboard panels** (left side of Setup): the dashboard has a left and a right column, with the process list below.

| Key | Action |
|---|---|
| `↑` `↓` | choose a panel |
| `Space` / `Enter` | show or hide it |
| `[` `]` or `Shift`+`↑` `↓` | move it up or down (it moves between columns at the ends) |
| `←` `→` | move it to the left or right column |
| `R`, then `Enter` or `R` | reset **all** settings to the defaults |
| `Esc` | back to the dashboard |

A live **Preview** shows where every panel will go. If a column ends up empty, the other one uses the full width.

**Appearance & behaviour** (use `←` `→` to change a value; each style shows a sample):

| Option | Choices |
|---|---|
| Process list | shown / hidden below the panels |
| Bar style | Blocks `████▌···`, Squares `■■■■■■`, Line `━━━━──`, Classic `[|||   ]` |
| Graphs | Blocks, Braille (twice the detail), Off (more room for everything else) |
| Left column width | Automatic or 35% to 70% |
| Refresh interval | 0.25 s to 10 s |
| Kernel threads | shown / hidden in the process list |
| Mouse | on / off |
| Network check | off / on: pings the router and the internet every 2 s (see [Network check](#network-check)) |

**Colours** (the Colours section of the settings list, same keys):

| Option | What it colours | Choices |
|---|---|---|
| Bars & values | bars, graphs and numbers | By value (green / yellow / red), Gradient (colour changes along the bar), Single colour, Monochrome |
| Colour | only in Single colour mode: the one colour used | cyan, blue, green, yellow, red, magenta, white, or the bright version of each |
| Highlight | the selection bar (process list, Setup, Logging), the active tab, the `hsm` badge and the key hints | same as Colour |
| Selected text | the text on that highlight | Automatic (black or white, whichever reads better), black, dark gray, white, bright white, cyan, blue, green, yellow, red, magenta, or a bright version |
| Titles | panel and box titles ("CPU", "Dashboard panels", "Sensors"...) and the group headings in the Logging view | Automatic (cyan) or any of the Selected text colours |
| Background | the whole screen | Terminal default (whatever your terminal uses), black, dark gray, blue, magenta, red, green, cyan, yellow, white or bright white |

Bright colours, dark gray and bright white need a 16-colour terminal; with `TERM=xterm` (8 colours) they are skipped.

On a coloured background, hsm adjusts the text so it stays readable: light text on dark backgrounds (with the bright versions of cyan, magenta and so on, so nothing disappears into blue or red), black text on light ones. Anything that would end up the same colour as the background, such as a cyan highlight on a cyan background, is drawn the other way round, and selected text that matches the highlight falls back to black or white.

**CPU cores**: every core gets a row, with a grey heading that names the columns:

```
# usage                         clock  temp
0 ███████████▌············  48%  3.6G   54°
1 ███▍····················  14%  3.4G   52°
2 ███████████████████████▊  97%  3.0G   71°
```

| Option | Choices |
|---|---|
| Bars show | Usage (how busy the core is), Temperature (0-100 °C, per core on Intel) or Clock speed (against the core's maximum). The `b` key changes it without opening Setup. |
| Other values | Shown: the two the bar doesn't show, as numbers next to it. Hidden: just the bar and its value. |

The numbers keep their colours (a hot core turns yellow, then red). On CPUs without a sensor per core (most AMD CPUs) the temperature column is left out, since every core would show the same package temperature. With many cores or a narrow panel, the numbers make room first, then the cores spread over more columns.

**Alerts** and **Logs** have their own sections; see [Catching failures](#catching-failures).

**Sensors**: *Sensor names and limits* (in the Sensors section of Setup) lists every sensor with its reading. `Enter` gives one a name of your own (*Front fan* instead of *nct6798 fan2*), `l` a limit of your own, `h` hides one that reads nonsense (an unconnected input showing 127 °C), and `d` undoes all three. Hidden sensors aren't even read, so hiding one that is slow to answer also stops it from holding hsm up. A reading that hasn't changed since hsm started while the others moved (ACPI's 27.8 °C placeholder, a USB-C port with nothing on it at 0 V) is marked *hasn't changed*, and `H` hides all of those at once. Your names appear in every panel, the Hardware view, events and new logs. Past your limit the value turns red (yellow 10 °C before it, for temperatures) and, after 5 seconds, it becomes an event, which the flight recorder and alerts follow like any other:

| Sensor | Your limit means |
|---|---|
| temperature | the most it should reach, in °C |
| fan | the slowest a working fan should turn, in RPM: a fan that slows down with age, dust or a worn bearing shows up long before it stops |
| voltage | the range it should stay in, like `1.10-1.40` (or one side: `1.10-`, `-1.40`) |
| power | the most it should draw, in W |

Sensors that are away (a USB device, a card that is out) keep their settings until they are back.

**Reset**: the settings list ends with *Reset ALL settings to defaults* (or press `R` anywhere in Setup). It asks you to press Enter a second time, so nothing is lost by accident; any other key cancels.

Any panel with spare height uses it for a history graph.

### The config file

Settings live in `~/.config/hsm/config` (or `$XDG_CONFIG_HOME/hsm/config`). It's plain text, so you can also edit it by hand:

```
left = cpu, gpu, health
right = memory, power, disk, network, temps, fans, voltages, services, filesystems
hidden = filesystems, voltages
processes = on
left_width = auto
bars = blocks          # blocks | squares | line | classic
graphs = braille       # blocks | braille | off
colors = gradient      # value | gradient | single | mono
core_bars = usage      # what the per-core bars show: usage | temp | clock
core_values = on       # the other two as numbers next to each bar
mouse = on
single_color = green   # used by colors = single; cyan blue green yellow red magenta white,
                       # or bright-cyan, bright-blue, ... bright-white
highlight = cyan       # selection bar, active tab, key hints; same colour names
selected_text = auto   # auto black dark-gray white bright-white cyan blue green yellow red magenta,
                       # or bright-cyan, bright-blue, ... bright-magenta
titles = auto          # auto (cyan) or any selected_text colour
background = default   # default black dark-gray blue magenta red green cyan yellow white bright-white
interval = 1
sort = cpu
reverse = off
kernel_threads = off
process_tree = off
log_dir = default      # or a folder, e.g. ~/hsm-logs
log_every = 5          # seconds between log rows, or "refresh" for every update
log_skip = net.eth0.rx, net.eth0.tx   # values left out of the system log
log_keep_days = 30     # delete logs older than this; forever = keep them all
recorder = on          # flight recorder
recorder_minutes = 10  # how far back it keeps: 2, 5, 10 or 30 (or 1-60 by hand)
alert_notify = desktop # desktop | bell | both | off
alert_cpu_temp = 90    # °C, or off
alert_gpu_temp = 90    # °C, or off
alert_gpu_slowing = on # a GPU slowing itself down from heat for minutes
alert_disk_ms = 500    # time per request, held for 10 s, or off
alert_low_memory = 5   # % of memory available, or off
alert_throttling = on
alert_disk_full = 95   # % of a file system's space (or inodes) used, or off
alert_problems = on    # freezes, kernel errors, out-of-memory kills, failing drives
alert_command =        # run on every alert by sh: $1 title, $2 details, $3 warning/error
sensor = coretemp/Package id 0 | name CPU | max 90     # your names and limits (Setup does this)
sensor = nct6798/fan2 | name Front fan | min 400
sensor = nct6798/in0 | min 1.1 | max 1.4
sensor = nct6798/AUXTIN3 | hide
net_check = off        # ping the router and an internet address every 2 s
net_check_host = default  # the internet address: default is 1.1.1.1; an IP address or a name
```

hsm saves the file when you change something in Setup, and on exit only if you changed something during that session, so hand edits aren't overwritten. When you run `sudo hsm`, it uses *your* config file (not root's) and reads and writes it as you, so the file stays owned by you.

## Logging

Logs help you find out what happened before a crash, a shutdown or a slowdown: how hot the CPU got, which process was using it, whether a fan stopped, how hard a disk was working. Everything is written as CSV, so you can open the files in LibreOffice Calc, Excel or Google Sheets, or plot them with any tool.

**System log.** Press `5` to open the **Logging** view. It lists every value hsm can record, grouped by CPU, CPU cores, Health, CPU idle, Memory, GPU, Power, Temperatures, Fans, Voltages, Disks, Network and Events, each with its current reading:

| Key | Action |
|---|---|
| `↑` `↓` `←` `→` `PgUp` `PgDn` | move through the list |
| `Space` / `Enter` | select or deselect a value |
| `a` / `n` | select all / none |
| `i` | time between rows: every refresh, 1, 2, 5, 10, 30 or 60 seconds |
| `l` | start / stop the log (this key works in every view) |
| `v` | the saved logs, and the log viewer (see [Viewing saved logs](#viewing-saved-logs)) |
| `r` | flight recorder on / off |
| `d` | delete logs after: never, then 365, 180, 90, 60, 30, 14 or 7 days |

Everything is selected at first. Your selection and interval are saved. While a log is running, the header shows a red **● REC** and the selection can't change, so every row has the same columns.

**Process logs.** Select a process in the Processes view (`2`) or the dashboard's process list and press `L`. Its row gets a red `●` and its usage goes to its own file. Press `L` on it again (or `Space` on it in the Logging view) to stop. Up to 16 processes can be logged at once. When a process exits, its log is closed and the footer tells you.

**Where the files go.** `~/.local/state/hsm/logs/` (or `$XDG_STATE_HOME/hsm/logs/`), or the folder set by `log_dir` in the config file, in one folder per day. Under `sudo hsm` they still go to *your* folder and belong to you. A new file is made each time you start a log; nothing is ever overwritten. A log that runs past midnight carries on in a new file in the next day's folder (with the column headings again), so every file belongs to one day.

```
logs/
  2026-11-11/
    hsm-system-2026-11-11_18-08-27.csv
    hsm-process-1288-firefox-2026-11-11_18-08-49.csv
  2026-11-12/
    hsm-system-2026-11-12_00-00-00.csv            the same log, after midnight
    hsm-incident-2026-11-12_01-23-40-thermal.csv  saved by the flight recorder
```

System log columns: `time` (your local time), `utc_offset`, `elapsed_s`, then one column per selected value with its unit in the heading:

```
time,utc_offset,elapsed_s,"CPU total (%)","CPU temperature (C)","Average CPU clock (MHz)","GPU load (%)",...
2026-11-11 18:08:27,+02:00,0.0,12.5,54.0,3602,8.0,...
```

The `utc_offset` column keeps the times correct when the clocks change: on the night summer time ends, 03:30 happens twice, once at `+03:00` and once at `+02:00`. (Logs from before 1.6 don't have this column; the log viewer reads both.)

**Deleting old logs.** Press `d` in the Logging view (or use *Delete logs after* in Setup) to choose how long logs are kept: 7, 14, 30, 60, 90, 180 or 365 days, or never deleted (the default). hsm then removes older ones when it starts and once an hour after that, as long as it is running (also as `--watch` or `--log`). Only its own `hsm-*.csv` files are deleted, a day folder only once it is empty, and nothing that a symbolic link points to. After you change the setting, the first clean-up waits 2 minutes, so stepping through the choices doesn't delete anything you wanted to keep.

Besides temperatures, fans, load and clocks, the system log can record everything from the Health panel (CPU throttled %, the CPU held back by its power limit, pressure, OOM kills, PCIe errors, the time the system stood still, NVMe wear / errors / unsafe shutdowns / time slowed by heat, SATA wear / bad sectors / cable errors), voltages, disk time per request, network errors and the Wi-Fi's signal, link speed and frequency, the network check's delays and lost pings (when it's on as the log starts), an NVIDIA GPU's video engines and PCIe link, and CPU idle states and wake-ups. The last column, **Events**, holds the text of anything that happened since the previous row (kernel errors, throttling, an OOM kill...), so the moment of a problem is easy to find in a long log.

Process log columns: `time`, `utc_offset`, `elapsed_s`, `pid`, `state`, CPU (% of one core), memory %, RSS (MB), threads, disk read and write (MB/s), GPU % and GPU memory (MB), open files, and the whole system's CPU % and CPU temperature next to it, so you can see whether the process was the cause. A steadily climbing open-files count is the classic sign of a file leak.

A value that can't be read at that moment (for example RAPL power without root) is left empty. Every row is written as soon as it is taken, and the file is synced to disk every 5 seconds, so a log survives a crash or power loss with at most the last few seconds missing.

**Logging in the background (no screen).** For tracking down a crash that happens overnight, run hsm headless:

```sh
hsm --log --log-every 5                      # the values chosen in the Logging view
hsm --log-pid 4321 --log-every 1             # one process; stops when it exits
sudo hsm --log --log-pid 4321                # with RAPL power; files still owned by you
nohup hsm --log --log-every 10 >/dev/null 2>&1 &   # keeps running after you log out
hsm --watch                                  # no log, just the flight recorder and alerts
hsm --watch --log --log-every 10             # both
```

It prints where the files are, writes until `Ctrl+C` or `kill`, then prints how many rows were saved. To start it at every boot, see [Running in the background](#running-in-the-background).

## Viewing saved logs

Press `v` in the Logging view (`5`) for the list of saved logs, newest first: when each one started, what kind it is (system, process, or an **incident** from the flight recorder, with what set it off) and its size. `Enter` (or a double click) opens one in the **log viewer**, and `hsm --view FILE` opens a file directly.

```
╭─ hsm-incident-2026-11-12_01-23-40-thermal.csv 01:13:40 → 01:24:39 · 10m 59s · 660 rows ──────────╮
│   CPU total              94.1% │ ▸ 01:23:40  the problem                            2m 44s shown │
│   CPU temperature       95.1°C │ CPU temperature (C)   95.1°C             scale 39.2°C – 102.2°C │
│   Package power         38.9 W │                            ▁▃▃▂▁▁▁▂▃▃▂▂▁▁▂▂▃▂▂▁▁▂▂▃▃▂▁▁▁        │
│   Load average (1 min)    7.84 │                    ▁▃▄▅▅▅▆▇█████████████████████████████▇       │
│   OOM kills since boot       0 │            ▁▃▅▆▆▆▇▇██████████████████████████████████████▆      │
│   Average CPU clock   3200 MHz │ ▁▁▁▂▃▅▆████████████████████████████████████████████████████     │
│                                │ ███████████████████████████████████████████████████████████▇    │
│                                │ █████████████████████████████████████████████████████████████▆  │
│                                │   ╵01:22:00              ╵01:23:00              ╵01:24:00       │
│                                │                                         ▲    ▲                  │
│                                │ Events (3)                                                      │
│                                │  01:23:40 thermal: CPU throttled (96 °C) while stress used 590% │
│                                │  01:23:52 thermal: CPU at 96 °C, above the alert limit of 90 °C │
│                                │  01:23:52 disk: sda: 2 new unreadable (pending) sectors         │
╰──────────────────────────────────────────────────────────────────────────────────────────────────╯
```

A log opens on its **Summary** (an incident log on the column the problem was about, below): the CPU's busiest moment, how hot it got and whether it throttled; each GPU's load, temperature and how long it sat near its hottest, its clock early and late in the busy time and how much of it was held back by heat or its power limit; power, memory, waiting, fans, the hottest other sensors, the busiest disks, freezes, gaps in the rows (hsm or the whole system standing still) and the events by kind. Lines worth a look are in yellow. `s` on it saves it as a text file next to the log, to keep or to share. When you stop a log with `l`, the message gives its gist: *CPU up to 86 °C, no throttling; GPU1 up to 87 °C, slowed by heat 61% of its busy time; 3 events*.

Below the summary is every column of the log, with its value at the cursor; the graph shows the selected one (the highest value of each time slice, so short spikes stay visible) with the cursor as a highlighted bar, and `Space` keeps up to three more stacked under it. `▲` marks the events, which are listed below. An incident log opens on the column the problem was about, with the cursor on the moment it happened, and counts the time from it.

| Key | Action |
|---|---|
| `↑` `↓` `PgUp` `PgDn` | the summary, or the column to graph |
| `←` `→` | move the cursor (a click in the graph puts it there) |
| `+` `-` (or the wheel) | zoom in / out around the cursor |
| `0` | the whole log |
| `[` `]` | scroll half a screen back / forward |
| `Home` `End` | the start / end of the log |
| `n` `N` | the next / previous event |
| `Space` | keep this graph on screen (again: remove it) |
| `s` | save what's on screen next to the log: the summary as text, graphs as an SVG picture |
| `Esc` | back to the list (`Esc` again: back to the Logging view) |

Very long logs are thinned out when loaded (keeping the highest value of every few rows), so even a log of several days opens in a moment.

**Pictures.** `s` saves the graphs on screen (the selected column and the ones kept with `Space`), over the time shown, as an SVG file next to the log, such as `hsm-incident-2026-11-12_01-23-40-thermal-cpu_temperature.svg`. It has the lowest, highest and average value of each graph, the events as dashed red lines (hover over one for its text) and listed below, and opens in any browser, so it can go into a report, a bug report or a forum post.

## Catching failures

Rare problems, like a freeze once a week or a reboot in the middle of the night, are the hardest to track down: by the time you look, it's over. hsm has five tools for them. They work in the normal screen and in the background (`hsm --watch`, see [Running in the background](#running-in-the-background)).

### Flight recorder

Like an aircraft's, it is always recording but only keeps the recent past: the last **10 minutes** of every value the Logging view lists, one row a second (or one per refresh, when the refresh is slower), in memory (a few hundred KB). When something goes wrong, those minutes are saved as an **incident log**, which keeps recording until a minute after the last problem:

* the CPU throttles because of heat,
* the whole system freezes for 2 seconds or more,
* a temperature reaches its limit, or the CPU or a sensor passes your limit,
* a fan stops while things are hot,
* memory runs out (an out-of-memory kill, or less than your alert limit left),
* a disk becomes very slow, stops answering, is almost full or turns read-only,
* the kernel reports an error (I/O errors, GPU hangs, machine checks, lockups...).

So you get the lead-up to a problem without logging around the clock. Incident logs are named after the moment and the kind of problem (`hsm-incident-2026-11-12_01-23-40-thermal.csv`), go into the day folders with the other logs, and have the same columns as a system log, except that the third one, `from_problem_s`, counts the seconds from the problem (negative before it). To keep a flaky device from filling the disk, a new incident log starts no sooner than 2 minutes after the last one ended and at most 30 times a day, and one is closed after 30 minutes at the latest.

A crash or power cut would take the recorder's memory with it, so it also writes the same rows to two small files that take turns (in `~/.local/state/hsm/sessions/`, under 1 MB together, synced to the disk every 15 seconds). If hsm doesn't get to close normally, the next start turns them into an incident log of the minutes before (`...-shutdown.csv` or `...-killed.csv`).

Switch it on or off with `r` in the Logging view, and choose how far back it keeps (2, 5, 10 or 30 minutes) in Setup. It is on by default.

### Freezes

A system that stands still for a few seconds and then carries on leaves nothing in any log: the mouse stops, the music stutters, and then it's as if nothing happened. hsm notices: a small thread wakes up ten times a second and checks how long it was really away. If it couldn't run for 2 seconds or more although the computer wasn't asleep, nothing else could run either. It doesn't read any files, so a slow disk alone doesn't fool it, and hsm itself being stopped (`Ctrl+Z`) doesn't count.

The event says how long it lasted and, from the pressure figures, what the system was busy with:

```
14:02:11  freeze   System froze for 3.2 s: memory was full (biggest: firefox, 5.2G)
14:31:40  freeze   System froze for 7.9 s: everything was waiting for a disk while rsync wrote 85.2M/s
16:05:02  freeze   System froze for 2.4 s
```

A freeze without a cause usually comes from a driver or the firmware (BIOS SMIs) holding the CPU, or a virtual machine's host. Freezes of 5 seconds or more are errors (and alerts); every one starts an incident log. Suspends are told apart and noted too, with what the battery lost meanwhile (*Woke up after 7h 12m asleep (suspended at 23:10, woke at 06:22); the battery went from 81% to 74%, 1.0% an hour (0.47 W)*): sleep that drains a laptop's battery is a common problem on Linux, and this shows it every morning. Suspends that failed are noted too (*Suspend failed at the suspend step, because of 0000:00:14.0*).

### Crash detection across reboots

While it runs, hsm keeps a small file with the current boot's ID and a "still alive" time, refreshed every 15 seconds and deleted when hsm closes normally (also when you log out or shut down normally). If the file is still there at the next start:

* from an earlier boot: the computer went down while hsm was running, without a normal shutdown: a crash, a freeze and reset, a power cut or holding the power button. hsm shows **"The last session ended unexpectedly"** with the last time it was seen alive.
* from this boot, but that hsm is gone: it was killed (for example by the out-of-memory killer) or crashed.

The notice also offers the incident log of the minutes before (`v` opens it in the log viewer). The Hardware view lists the last unexpected ends, and they are kept in `~/.local/state/hsm/unexpected-ends.log`.

Many computers also keep the **kernel's own crash report** across a reboot, in the firmware's storage (pstore). After a kernel panic, oops or lockup, hsm shows the most telling line (such as `Kernel panic - not syncing: Fatal exception in interrupt`) and where the full report is. Each report is shown once. On systemd distributions, `systemd-pstore` moves the reports to `/var/lib/systemd/pstore` at boot, which only root can read, so run `sudo hsm` once after a crash to see them.

### Who was busy

Events name the busiest process at that moment, so the cause is often right there:

```
01:23:40  thermal  CPU throttled (96 °C) while stress used 590% CPU
01:31:02  memory   Memory ran out: the kernel killed 1 process (biggest: chrome, 7.1G)
01:40:15  disk     sda very slow: 1240 ms per request for 10 s while rsync wrote 85.2M/s
```

CPU problems name the process using the most CPU (from 15%), memory problems the one using the most memory, disk problems the busiest reader or writer (from 1 MB/s), and GPU temperatures the heaviest GPU user.

### Alerts

When a limit is crossed, hsm can tell you even while its window is hidden. Set it up in the **Alerts** section of Setup:

| Setting | Default | Alerts when |
|---|---|---|
| Alert by | Desktop | how: a desktop notification, the terminal bell, both, or off (the limits below still count as problems for the Health panel and the flight recorder) |
| CPU hotter than | 90 °C | the CPU stays above it for 5 seconds |
| CPU throttling | on | the CPU slows down because of heat |
| GPU hotter than | 90 °C | a GPU stays above it for 5 seconds |
| GPU slowing | on | a GPU has slowed itself down from heat for about 3 minutes (short breaks don't reset the count; NVIDIA). It comes back after a proper break. |
| Disk slower than | 500 ms | a drive takes longer than this per request for 10 seconds, or stops answering |
| Memory left below | 5% | less memory than this is available, for 5 seconds |
| Disk fuller than | 95% | a file system has used this much of its space, or of its inodes (the number of files it can hold) |
| Problems | on | freezes of 5 s or more, fans that stopped, kernel errors, out-of-memory kills, failing drives, a disk turned read-only, a crash found at start |

*Send a test alert* (at the end of the section) checks that notifications reach you. Each kind of alert waits 5 minutes before it repeats, and a temperature alert only comes back once the CPU (or the GPU) has cooled down by 5 °C. Every alert is also an event in the Health panel and the logs (and starts an incident log).

Desktop notifications go through `notify-send`, run as you also when hsm runs with `sudo`, so they appear on your desktop. When hsm runs as root without sudo (a system service), they go to everyone logged in to a desktop. In the background (`--watch`), alerts are also printed, so they end up in the service's journal.

For anything else, such as a message to your phone, set `alert_command` in the config file. It is run by `sh` on every alert (as you, not as root, when hsm runs with `sudo`), with the title in `$1`, the details in `$2` and `warning` or `error` in `$3`:

```
alert_command = curl -s -d "$1: $2" ntfy.sh/my-hsm-alerts
```

## Running in the background

`hsm --watch` runs the flight recorder, alerts, freeze and crash detection, the [events and daily history](#events-and-history) and log clean-up without a screen, until `Ctrl+C` or `kill`. With `--log` it also writes a regular system log. A systemd **user** service starts it with your desktop session, so its desktop alerts reach you (`~/.config/systemd/user/hsm-watch.service`, then `systemctl --user enable --now hsm-watch`):

```ini
[Unit]
Description=hsm flight recorder and alerts

[Service]
ExecStart=/usr/local/bin/hsm --watch
Restart=on-failure

[Install]
WantedBy=default.target
```

To also have power readings (RAPL), the kernel log and drive health, run it as root instead, as a system service (`/etc/systemd/system/hsm-watch.service`, then `sudo systemctl enable --now hsm-watch`). Its files then go to root's folders, or to the one you name:

```ini
[Unit]
Description=hsm flight recorder, alerts and log

[Service]
ExecStart=/usr/local/bin/hsm --watch --log --log-every 10 --log-dir /var/log/hsm
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Set *Delete logs after* (or `log_keep_days` in that user's config file) so a log running for months doesn't fill the disk.

### Serving metrics (Prometheus, Grafana)

`hsm --serve 9101` answers over HTTP while it runs:

| Address | What |
|---|---|
| `/metrics` | Prometheus text format: the machine (`hsm_machine_info`), CPU (total and per core), clock, temperature, load, its power limits and `hsm_cpu_power_limited`, memory and swap, GPUs (with `hsm_gpu_slowed_down{reason="heat"}`, `hsm_gpu_asleep`, `hsm_gpu_video_percent` and `hsm_gpu_pcie_link` for NVIDIA), power and RAPL zones, batteries, every sensor (with your names), disk and network byte counters, disk busy % and time per request, file systems, links up and dropped, pressure, throttling, PCIe errors, freezes, runaway processes, kernel errors, drive wear, media errors and bad sectors, NVMe heat slowdowns (`hsm_drive_heat_slowdowns_total`, `hsm_drive_heat_slowed_seconds_total`), swap devices and zram (`hsm_swap_device_bytes`, `hsm_zram_bytes`), the Wi-Fi's signal, link speed and frequency, USB devices and slow ones (`hsm_usb_devices`, `hsm_usb_slow_devices`), the network check's `hsm_ping_seconds` and `hsm_ping_loss_ratio`, and `hsm_health_status` (0 fine, 1 warning, 2 problem, as the badge in the header) |
| `/json` | the same as `hsm --json` |
| `/` | a short page with the links and the state right now |

It listens on this machine only (`127.0.0.1`) unless you give an address: `--serve 0.0.0.0:9101` (or `[::]:9101`) opens it to the network. There is no password, so do that only where anyone who can reach the port may read the figures. It only reads, answers `GET` and `HEAD`, and gives each client 2 seconds at most. Combine it with `--watch` (and `--log`) to have everything in one service: `ExecStart=/usr/local/bin/hsm --watch --serve 9101`. For Prometheus:

```yaml
scrape_configs:
  - job_name: hsm
    static_configs:
      - targets: ["localhost:9101"]
```

## Health: finding the cause of a failure

The **Health** panel (on by default, below the GPU) and the top of the **Hardware** view (`3`) collect the signs that usually come before a freeze, crash or reboot. While something is wrong right now, the header shows an alert (such as **⚠ throttling**) in every view.

| Shown as | What it means | What's normal |
|---|---|---|
| **Waiting** CPU / Mem / I/O | The kernel's *pressure stall information* (PSI): the share of time tasks had to wait for the CPU, for memory, or for the disk. This is the best single measure of "the computer is struggling", better than CPU % or load average. The Hardware view also shows the kernel's 10 s / 1 min / 5 min averages and the "full" figure (every task waiting at once). | under 10%. Memory pressure above a few % means it is swapping or reclaiming hard. |
| **Throttling** | The CPU slowing itself down because it reached its temperature limit, counted by the CPU itself (Intel). Shows "NOW" while it happens, how often since hsm started, and since boot. | none. Some at heavy load on a small case or laptop is common; often means dust, dried thermal paste or a weak fan. |
| **Power limit** | The CPU held back by its [power limit](#cpu-power-limits) rather than by heat: it draws what the limit allows while its clock stays below what it can do. Shown while it happens and as the share of its busy time since start; needs root. | common on laptops under sustained load, where the CPU and GPU share one budget |
| **GPU** | NVIDIA GPUs: holding its clock back now, by heat or by its power limit, or how much of its busy time it did since hsm started (see [NVIDIA GPUs on laptops](#nvidia-gpus-on-laptops)). | *full speed*. A laptop GPU under heavy load often sits at its temperature limit. |
| **OOM kills** | Processes the kernel killed because memory ran out (the reason behind apps that "crash for no reason"). | none |
| **PCIe** | Errors on the PCIe bus, counted per device (AER). Corrected errors are retried by the hardware; uncorrected ones lose data. | none. A slowly growing corrected count points at a failing drive, GPU, riser or slot, or a power problem. |
| **Drives**: NVMe | The drive's own health log: wear used against its rating, spare blocks, media / data integrity errors, unsafe shutdowns (power lost or a crash), hours powered on, time spent too hot, and how often and how long it slowed itself down to cool off (its thermal management, light and hard). A long copy or a game install on a hot laptop shows up there: *nvme0 slowed by heat* in the panel while it goes on, an event when it starts and a summary when it's over (*slowed 1m 30s (20s of it hard) within 2m 00s, up to 81 °C*). | 0 media errors, wear well below 100%. Unsafe shutdowns go up by one with every crash or power cut. Heat slowdowns: rare; in a thin laptop during long writes, which then run slower. |
| **Drives**: SATA | 2.5" and 3.5" SSDs and hard disks: the SMART attributes the drive keeps about itself. Reallocated, pending (unreadable) and uncorrectable sectors, cable errors (UDMA CRC), wear used (SSDs), hours powered on, power cycles and power losses, temperature, and "FAILING" when an attribute reaches the maker's limit. A hard disk that has spun down is left asleep: hsm asks for its power state first and only reads it while it spins. USB enclosures and RAID controllers are skipped. | 0 pending and uncorrectable sectors. A few reallocated sectors that don't increase are survivable; a growing number means replace the drive. Cable errors that keep growing mean a bad SATA cable or connector. |
| **Kernel** | Kernel messages about hardware and crashes: I/O errors, file system errors, GPU hangs and resets, machine checks, lockups, segfaults, under-voltage, suspend and resume. USB devices that fail to connect are warnings, not errors (a cable or a port, not the computer), and don't start an incident log. | a few harmless firmware (ACPI) complaints at boot |
| **Battery** | Shown once a laptop's battery holds less than 80% of what it did when new. | not shown |
| **Froze** | The whole system stood still for 2 s or more (see [Freezes](#freezes)); shown once it happened. | never |
| *fan* events | A fan that was turning stopped while the CPU (or, for a graphics card's fan, the card) is at 70 °C or more. Fans that rest when it's cool, and headers with nothing plugged in, don't count. | never |
| *disk* events | A file system almost full (your *Disk fuller than* limit, space or inodes), or one that turned read-only: ext4 and others do that after disk errors, and from then on every write fails. | never |
| *net* events | A cable or Wi-Fi link lost, back after how long, or dropped for a moment (Linux 4.16+), and Wi-Fi moving to another band (5 GHz to 2.4 GHz is slower and more crowded). Links switched off on purpose and the time around a suspend don't count. With the [network check](#network-check) on, also pings lost or far slower than usual: to the router (Wi-Fi or cable trouble, with what the Wi-Fi was doing then) or only to the internet (beyond your network). | rare |
| *usb* events | A USB device connected (with its kind, speed and port) or disconnected. One that drops off and comes back within 3 seconds (a loose plug or cable, too little power) is a warning, and so is a USB 3 drive or network adapter that connects at USB 2 speed. The seconds after waking from sleep, when everything reconnects, stay quiet. The kernel's own USB errors are here too. See [USB devices](#usb-devices). | drops: never |
| *slow* events | One of hsm's readings took a second or more, and the screen waited for it: which sensor chip, GPU, process or check. On laptops it's usually the firmware's sensors (fans, battery); hiding that sensor in Setup stops hsm from reading it. When the whole system stood still, that's reported as a freeze instead. | never |
| *leak* events | A process whose memory only ever grows: by 256 MB and by a third at least, over half an hour or more, hardly ever giving any back. Normal programs go up and down. Its RSS gets a `↑` in the process list, and its details page shows the last two hours. | none, or a program to keep an eye on |
| *runaway* events | A process that has kept a whole core busy for 15 minutes without a break: a stuck browser tab, something left running after a game closed, an indexer that never finishes. It costs battery and fan noise. Programs using the GPU don't count, so games and video don't set it off, and names stay as the program gives them. Its CPU % gets a `!` in the process list, and its details page says how long. | none |
| *events* | The latest of all of the above, newest first, with the time. Hsm adds its own: throttling started, memory ran out, new PCIe errors, a temperature reaching its limit, new bad sectors or cable errors, alerts, an unexpected end of the last session, the CPU governor changing. Problems name the [busiest process](#who-was-busy) at that moment. | |

Three of these need root (run `sudo hsm`): the kernel log (most distributions restrict it), the NVMe health log (with its heat slowdowns) and SATA drive health. Everything else works as a normal user. Your own limits for sensors (see *Sensors* in [Customizing](#customizing-the-dashboard)) add events of their own. Throttling counters exist on Intel CPUs; AMD CPUs throttle too but don't report it this way.

All of it can be recorded: see [Logging](#logging), which also writes the events as text into the log, in the row where they happened.

**After a crash**: if hsm was running, it says so at the next start and has saved the minutes before (see [Catching failures](#catching-failures)); the log viewer shows them. Then `journalctl -b -1 -e` shows the end of the previous boot's system log, with the kernel messages from the moment it went down.

## Network check

Off until you switch it on (Setup, Behaviour: *Network check*, or `net_check = on` in the config file). hsm then pings your router (the IPv4 default route's gateway) and one internet address (1.1.1.1 unless you set `net_check_host`) every 2 seconds:

* the **Network** panel shows the delay to each and the share of pings lost over the last minute (*ping router 3.2 ms · internet 18 ms*), the Hardware view the average, the range and the jitter (how much the delay varies);
* comparing the two tells where a problem is: when the router itself answers late or not at all, it's your Wi-Fi or cable; when only the internet does, it's beyond your network (the provider, or the line);
* trouble is an event when it starts (3 of the last 10 pings lost, or the delay far above usual, over 100 ms) and when it's over, with how long it lasted, e.g. *Wi-Fi trouble: 40% of the pings to the router are lost*. That also explains lag spikes in games;
* logs started while it runs get four columns (the delay to each, and the losses), and `/metrics` and `/json` have them too.

Each ping is 44 bytes, so the check costs nothing worth mentioning, but it does send packets every 2 seconds, which is why it's off unless you ask for it. It uses the kernel's unprivileged ping sockets (see [Permissions](#permissions-what-needs-root)); the network check itself runs in a small thread of its own.

### Wi-Fi details

For each Wi-Fi interface, hsm asks the kernel (nl80211, as `iw dev wlan0 link` does; no root needed) for the band and channel it's on, the channel's width, the link speed each way and the Wi-Fi generation. The Network panel shows them next to the signal (*-58 dBm  5 GHz ch 36  867M*), the Hardware view in full (*Wi-Fi 6, 5 GHz channel 36, 80 MHz wide, signal -58 dBm (70%)* and *link 867 Mb/s out, 780 Mb/s in, 3% sent again, connected 2h 05m*). Together with the network check that explains most lag: router trouble events say what the Wi-Fi was doing then (*Wi-Fi trouble: 40% of the pings to the router are lost (192.168.1.1); Wi-Fi signal -78 dBm, 2.4 GHz channel 6, 72 Mb/s*), and moving to another band is an event of its own. A link speed under about 30 Mb/s, or a high share of frames sent again, means a weak or crowded connection. Logs get the link speed and the frequency, `--json` a `wifi` object for each wireless interface, and `/metrics` has them too.

## USB devices

The Hardware view lists what's plugged in over USB, read from `/sys` (no root needed):

```
Port  Device                         Kind           Speed      Where
1-1   SanDisk Ultra                  storage sdb    USB 2 480M socket
      runs at USB 2 speed, can do 5G: a USB 2 cable, or the plug not in all the way?
1-3   Logitech USB Receiver          keyboard/mouse USB 1 12M  socket
1-7   Chicony Integrated Camera      camera         USB 2 480M built in
2-2   Samsung PSSD T7                storage sdc    USB 3 10G  socket
```

*Port* is where it's plugged in: the bus, then the port (`1-4.2` is port 2 of a hub in port 4). *Where* tells a socket on the outside from a device built into the laptop, or one behind a hub. A USB 3 drive running at USB 2 speed is a common reason for slow copies (USB 2 manages about 40 MB/s): hsm compares what the device says it can do (its BOS descriptor, which Linux 6.10 and later show) with the speed it runs at, and says why when it can tell: a USB 2 hub on the way, a USB 2-only port, or a USB 3 port with a USB 2 cable or a plug not pushed in all the way. Such a drive also shows in the Health panel (*USB sdb at USB 2 speed*) and as a warning when it connects. Connecting, disconnecting and dropping off are [events](#health-finding-the-cause-of-a-failure); `--json` has a `usb` list and `/metrics` the number of devices and of slow ones.

## Process details

`Enter` (or a double click) on a process, in the Processes view or the dashboard's list, opens everything about it:

* where it comes from: the whole command line, the program file, when it started and by whom, its parent (click it to go there), its service or container, its working folder, state, threads and nice value;
* what it uses: CPU now, in all and on average, with a graph since the page opened; memory in RAM split into its own, files and shared, virtual size, its highest ever, swapped out, and its OOM score (the higher, the sooner the kernel kills it when memory runs out);
* how long it has kept a whole core busy without a break, once that's 5 minutes or more;
* growth: how its memory changed over the last two hours, once it uses 64 MB or more, and whether it only ever grows (a leak, or a cache without a limit);
* disk read and write now and in all, GPU use and memory, open files by kind (files, sockets, pipes) against its limit, and how often it waits or gets taken off the CPU;
* the processes it started (click one to open it).

`x`, `N` and `L` work on the process shown; `Esc` goes back. Some details of other users' processes need root.

## Events and history

**Events** (view `7`) keeps everything hsm noticed, across runs and reboots, in `~/.local/state/hsm/events.log`: throttling, freezes, suspends, kernel errors, full disks, dropped links, finished drive self-tests, cooling checks, your alerts... grouped by day, newest first, with the counts of the last day and week at the top. `/` searches the text and the category, `e` switches between all, warnings and errors, and errors only; the selected event is shown in full at the bottom, with what it means in plain words for the cryptic ones (USB devices that fail to connect or drop off, and whether the port is an outside socket or built in; a USB 3 drive at USB 2 speed; NVIDIA Xid codes, segfaults, I/O errors, lockups, slow readings...). The same line repeated right away, as the kernel does when it retries, is kept once: *... (3 times)*. Kernel messages from before hsm started are added once, not at every start. The file is plain text with tabs (time, level, category, text), starts over as `events.log.1` at 4 MB, and stays readable by other tools.

**History** (view `8`) keeps one line per day in `~/.local/state/hsm/history.csv`: the hours some hsm was running, CPU use and temperature (average and highest), memory, swap, power, gigabytes read, written, received and sent, the hottest GPU and drive (the drive's own sensor when its health log needs root), time throttled, freezes, out-of-memory kills, errors and warnings, the most worn drive, the fullest file system and the battery's health. The view shows the days, newest first, and above them how things changed: a column per day for the last two months, and the last week against the same week a month before, so *CPU temp, peak: last week 79 °C, a month before 72 °C (+7)* tells you to clean the fan before it turns into throttling. Drive wear also gets an estimate of when it reaches 100% at its current pace.

**What ran each day.** `↑` `↓` (or a click) select a day, and below the list hsm shows what ran on it: the programs that used the most CPU time, GPU time and disk writes, so *what kept the fans going yesterday?* has an answer. Programs are grouped by the name the kernel knows them by, unchanged (a game's process, `firefox`, `baloo_file`); kernel threads are left out, and a program's writes include those of short-lived helpers it started. The hsm that keeps the history counts them, and `programs.csv` (next to `history.csv`, a year's worth) keeps the top ten of each.

Only one hsm writes these files at a time: with a `hsm --watch` service running, an open screen leaves the writing to it, and takes over within a minute if the service stops. A day covers only the hours some hsm was running, so a service gives you whole days. Today's line is saved every 10 minutes and when hsm closes.

## Drive self-tests

`D` in the Hardware view lists your NVMe and SATA drives with what they keep about themselves:

* the **error log**: how many errors the drive reported in its life and what the latest one was. On NVMe drives most of them are harmless (*invalid field in a command*: a program asked for something the drive doesn't do); a *media error* or an *uncorrectable read error* is the kind that counts;
* the **self-tests**: the last ones with their result and when they ran (at how many power-on hours), and the one running now with its progress.

`s` starts a short self-test (a minute or two: the electronics and part of the surface), `e` an extended one (everything: minutes on an SSD, hours on a big hard disk), `a` stops a running one. hsm asks first. The test runs inside the drive, which keeps working normally meanwhile; hsm follows the tests it started and adds an event when they end (an error, if a test failed, with the first bad sector). This needs root, like `smartctl -t`.

## Cooling check

`C` in the Hardware view, or `hsm --cooling-check` on the command line, checks whether the cooling keeps up: every CPU thread is kept busy for a minute, then the CPU rests for half a minute, while hsm watches the temperature, the clock speed, the fans and throttling. The verdict:

```
Cooling keeps up: 45 °C to 78 °C under full load without slowing down, back to 47 °C after 30 s of rest.
The check of 12 Aug 2026 peaked at 71 °C: +7 °C now, the cooling got worse (dust?).
```

It says *gets hot* within 10 °C of the CPU's limit (or from 90 °C), *can't keep up* when the CPU throttles (or the check has to stop early, 3 °C before the limit, which it does by itself), and notes fans that didn't speed up. Each result is kept in `~/.local/state/hsm/cooling.log` and compared with the previous one, so running it every few months shows whether the cooling is getting worse. Close heavy programs first, and on a laptop plug in the charger. While it runs, a view key, a click on a tab or `q` asks first (*The cooling check is running. Press 1 again to stop the check and leave, any other key keeps it going.*): the check only stops when you mean it. `hsm --cooling-check` exits with 1 when the cooling can't keep up, 2 without a temperature sensor.

## Services and containers

View `6` lists every systemd **service**, login **session**, desktop **app** and **container** (Docker, Podman, Kubernetes, LXC) with its CPU (% of one core, like processes), memory, disk read and write rates, number of tasks, out-of-memory kills, and where it sits in the cgroup tree. Unlike the process list, a service's numbers include all of its processes, so "which service is eating my memory?" has a direct answer.

| Key | Action |
|---|---|
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | move |
| `Enter` | show the processes of the selected service in the process view (`Esc` shows all again) |
| `c` `m` `d` `n` `t` | sort by CPU, memory, disk I/O, name, tasks (again: reverse) |
| `/` | filter by name |

The **Services** dashboard panel (off by default; turn it on in Setup) shows the busiest five. This needs cgroup v2, which systemd uses by default since 2021 (Ubuntu 21.10, Debian 11, Fedora 31, Arch). The numbers are only collected while services are on screen.

## Process tree, priority and signals

**Tree.** Press `T` (or `F5`) in the process list to see which process started which: children are listed under their parent, with lines showing the family, and brothers in the current sort order. With a filter, the parents of each match stay in view (greyed) so you can see where it came from; the selection skips them. Press `T` again for the plain list. Your choice is saved.

**Priority (nice).** Select a process and press `N`: move the marker with `←` `→` (or click on the scale) between -20 (highest priority: it gets the CPU first) and 19 (lowest: it gives way to everything else), then `Enter`. `F7` and `F8` raise and lower it one step without the dialog. hsm changes every thread of the process, as `renice` does. Lowering a priority works for your own processes; raising one needs root (`sudo hsm`).

**Signals.** Select a process in the dashboard's process list or the Processes view (`2`), press `x` and choose a signal:

| Signal | What it does |
|---|---|
| SIGTERM | asks the program to quit (normal, safe default) |
| SIGKILL | forces it to stop immediately; it can't clean up |
| SIGINT | interrupt, like pressing Ctrl+C |
| SIGHUP | "hang up"; many services reload their config |
| SIGSTOP | pauses (freezes) the process |
| SIGCONT | resumes a paused process |

Before sending, hsm checks that the PID still belongs to the same process you selected (PIDs get reused when programs exit). The footer shows the result, for example "Permission denied" if the process belongs to another user.

## JSON output

`hsm --json` prints one snapshot of everything as JSON and exits, for scripts and other tools: the machine (model, board, BIOS), the CPU (with every core, and its `power_limit`), memory, GPUs (NVIDIA ones with `state`, `temp_limit_c`, why they're slowing down, their video engines and PCIe link), power, drives and file systems, network, sensors (with your names and limits), health (pressure, throttling, PCIe errors, every drive's health, the recent events), services and processes (all of them, or the busiest `-n N` by CPU; each with `busy_minutes` and `runaway`), USB devices (`usb`), each wireless interface's `wifi` (band, channel, width, link speed both ways), the swap devices with zram's compression (`memory.swaps`), each GPU's `screens` and `kept_awake_by`, and each NVMe drive's `heat_slowdowns`. With the network check on, `network_check` has the router's and the internet's delay and losses (`/json` only: a single snapshot has nothing to compare). The same comes from `/json` of [`hsm --serve`](#serving-metrics-prometheus-grafana), where `health.since_start` also counts freezes, suspends, dropped links and stopped fans. Units are in the key names (`_pct`, `_c`, `_mhz`, `_w`, `_bytes`, `_bps`, `_ms`), and anything the machine doesn't report is `null`.

```sh
hsm --json | jq '.cpu.temp_c'
hsm --json -n 5 | jq -r '.processes[] | "\(.cpu_pct)% \(.name)"'
hsm --json | jq '.health.drives[] | select(.type != "nvme") | {name, reallocated_sectors, pending_sectors}'
```

## Permissions: what needs root

Most things work as a normal user. A few are restricted by the kernel:

| Reading | Why | Options |
|---|---|---|
| CPU / DRAM / iGPU power (RAPL), and so the total | `energy_uj` is root-only since Linux 5.10 (PLATYPUS power side channel, CVE-2020-8694) | Run `sudo hsm`. See below for a permanent change. Without it there is no total (the GPUs alone would pass for one). |
| Exact Intel GPU engine load | system-wide perf counters need `CAP_PERFMON` or `perf_event_paranoid <= 0` | Run `sudo hsm`, or `sudo setcap cap_perfmon=ep /usr/local/bin/hsm`. Without either, it falls back to the RC6 estimate. |
| Disk I/O of other users' processes | `/proc/PID/io` is protected like ptrace | Run `sudo hsm`. Your own processes always show I/O. |
| Signals to other users' processes | normal Unix permissions | Run `sudo hsm`. |
| Kernel messages | `kernel.dmesg_restrict = 1` on most distributions | Run `sudo hsm`. |
| NVMe and SATA drive health (NVMe heat slowdowns too), self-tests and error logs | the drive's device node is root-only | Run `sudo hsm`. |
| Kernel crash reports saved by systemd | `/var/lib/systemd/pstore` is root-only | Run `sudo hsm` once after a crash. Reports still in `/sys/fs/pstore` are shown to everyone who can read them. |
| Raising a process's priority (negative nice) | needs `CAP_SYS_NICE` | Run `sudo hsm`. Lowering your own processes' priority always works. |
| When the power limit holds the CPU back | needs the package's power (RAPL, above) | Run `sudo hsm`. The limits themselves show for everyone. |
| The network check's pings | ping sockets are allowed for the groups in `net.ipv4.ping_group_range` (everyone on most distributions) | Otherwise run `sudo hsm`, which uses a raw socket instead, or widen the range: `sudo sysctl net.ipv4.ping_group_range="0 2147483647"`. |
| Open files and GPU use of other users' processes | protected like ptrace | Run `sudo hsm`. Your own processes always show them. |

**Making RAPL readable permanently (optional, single-user machines only).** This re-opens the side channel the kernel restriction closes, so only do it on a machine where you are the only user. Create `/etc/tmpfiles.d/hsm-rapl.conf`:

```
z /sys/class/powercap/intel-rapl:*/energy_uj   0444 - - -
z /sys/class/powercap/intel-rapl:*:*/energy_uj 0444 - - -
```

Then run `sudo systemd-tmpfiles --create` (it is also applied at every boot).

**"unknown terminal type"?** Your `TERM` names a terminal ncurses doesn't know. Run `TERM=xterm-256color hsm`, or install your distribution's `ncurses-term` package. A terminal that can't move the cursor at all (`TERM=dumb`, such as an editor's built-in shell) can't show hsm's screen: `hsm --once` prints a text snapshot instead.

**No fans in the Fans panel?** Many desktop boards only report fan speeds once their Super I/O sensor driver is loaded. Install `lm-sensors` and run `sudo sensors-detect`.

## How power is estimated

The Power panel title shows the best whole-machine figure it can find, in this order:

1. **Battery drain** when a laptop is discharging. This is the true total for the whole system.
2. **RAPL psys** (platform domain), on recent Intel laptops - unless it reads less than the CPU package inside it: some laptop firmware leaves it unconnected (0.08 W next to an 8 W CPU), so then it's left out, and the Hardware view says so.
3. **CPU package + DRAM + discrete GPUs**. Integrated GPUs are already inside the package figure, so they aren't added twice. A sleeping NVIDIA GPU counts as 0 W. This leaves out the motherboard, drives, fans and PSU losses, so wall power will be higher.

Without the CPU part (RAPL needs root) there is no total: the title says *total needs root*, the power graph shows the GPUs (labelled so), and logs and the daily history leave the total empty rather than record the GPUs as if they were everything. Each power meter keeps the time of its own last read, so a sample that was held up can't turn into a fake spike of power.

### CPU power limits

A CPU may draw only so much power: a **sustained** limit, which its power averaged over a time window (often 28 s) has to stay under, and a higher one for **short boosts**. Laptop makers set them to what the cooling and the power supply can take, and change them with the performance profile (the ASUS and Lenovo keys, `powerprofilesctl`). The Hardware view shows them under the CPU's power settings, and so do `--once`, `--json` and `/metrics`; reading them needs no root. Some firmware keeps a second set of its own (through MMIO): then both are shown, and the lower one is what counts.

With the CPU package's power (root), hsm also says when the limit, not heat, holds the CPU back: it draws what the limit allows while its clock stays below what it can do. The CPU panel then shows *slowing: power limit* next to the clock (*slowing: heat* while it throttles), the Health panel *Power limit: holding the CPU back*, and the Hardware view how much of its busy time it spent there. On a gaming laptop the CPU and GPU share one power budget, so this explains why the CPU clock drops in games although it isn't hot. A log can record it (*CPU held back by power limit*), and the log viewer's summary counts it.

## Project layout

```
src/main.c        argument parsing, main loop (sampling + input on one thread, no busy-waiting), headless --log /
                  --watch / --serve, --cooling-check
src/config.c      settings file: load, save, defaults (sudo-safe)
src/sys.c         ties the collectors together, history buffers, total-power estimate, the machine (DMI)
src/cpu.c         /proc/stat, cpufreq, topology, /proc/cpuinfo, load, uptime
src/mem.c         /proc/meminfo, /proc/vmstat, swap devices, zram, zswap
src/disk.c        /proc/diskstats (physical drives only), filesystems
src/net.c         /proc/net/dev, link state, addresses, Wi-Fi signal
src/wifi.c        Wi-Fi details from nl80211 (generic netlink): band, channel, width, link speed
src/usb.c         USB devices: what, how fast, from which port; plugged in and out
src/proc.c        process table, per-process CPU / memory / disk I/O / GPU (DRM fdinfo) / open files, safe signals
src/sensors.c     hwmon temperatures, fans, power, voltages; per-core temperature lookup
src/health.c      throttling, pressure stalls, OOM kills, PCIe AER, NVMe health log, kernel messages, freezes,
                  fans, links, read-only disks, your sensor limits, events, blame
src/freeze.c      freeze detection: a watchdog thread that notices when nothing could run, and suspends
src/leak.c        processes whose memory keeps growing
src/runaway.c     processes that keep a whole core busy for a long time
src/netcheck.c    the network check: a thread that pings the router and the internet
src/ata.c         SATA drive health: SMART attributes through ATA pass-through, without waking sleeping disks
src/selftest.c    the drives' self-tests and error logs (NVMe admin commands, ATA SMART logs)
src/cooling.c     the cooling check: load threads, measurements, verdict, cooling.log
src/cgroup.c      services and containers from the cgroup v2 tree
src/gpu.c         Intel i915 (perf PMU + RC6), AMD amdgpu, NVIDIA NVML via dlopen, generic hwmon, ARM boards' GPUs,
                  the screens on each GPU's ports (DRM connectors, EDID)
src/power.c       RAPL powercap zones, batteries, AC adapter
src/ui.c          header, footer, help, signal and priority dialogs, start-up notice, keys, mouse, view switching
src/ui_prim.c     drawing primitives: text, boxes, bar styles, block/braille graphs, colours, background, click areas
src/ui_panels.c   dashboard panels (CPU cores, Health...) and the two-column layout engine
src/ui_procs.c    process table: sorting, filtering, selection, tree
src/ui_hw.c       Hardware view
src/ui_setup.c    Setup screen: panels, styles, colours, CPU cores, alerts, logs, reset
src/ui_log.c      Logging view: choose values, start / stop logs, process logs, recorder and clean-up
src/ui_logview.c  saved logs: the list, and the viewer (CSV loader, graphs, events, SVG pictures)
src/ui_svc.c      Services view and panel
src/ui_pdetail.c  process details page (Enter)
src/ui_events.c   Events view (7) and History view (8), with what ran on the selected day
src/ui_sensors.c  Setup > Sensor names and limits
src/ui_drives.c   Hardware view > D: drive self-tests
src/ui_cool.c     Hardware view > C: the cooling check
src/log.c         logging: the list of values, CSV system and process logs, day folders, clean-up (sudo-safe)
src/watch.c       what runs after every sample, with or without the screen: the ones below and log clean-up
src/history.c     events.log and history.csv: one writer at a time, daily figures
src/programs.c    what ran each day: CPU time, GPU time and writes per program, programs.csv
src/recorder.c    flight recorder: the last minutes in memory and in rolling files, incident logs
src/session.c     crash detection: session heartbeat, unexpected ends, pstore crash reports
src/alert.c       alerts: limits, notify-send as the logged-in user, alert_command, terminal bell
src/report.c      --once plain-text snapshot
src/json.c        --json snapshot
src/serve.c       --serve: a small read-only HTTP server, Prometheus metrics
src/util.c        file reading, formatting, history ring buffer
```

Every collector keeps its own previous counters and turns them into rates using the real elapsed time between samples, so rates stay correct even if a frame is late.

### Testing against fake hardware

For development, `HSM_SYSFS`, `HSM_PROCFS`, `HSM_DEVFS` and `HSM_PCI_IDS` point the program at a different `/sys`, `/proc`, `/dev` (kernel log, NVMe and SATA drives) or `pci.ids`, and `HSM_PSTORE` at a different folder of saved crash reports. That lets you test GPU, RAPL, battery, fan, sensor and drive code on machines without that hardware: a SATA drive there is a plain file holding its 512-byte SMART data, the 512-byte thresholds, and optionally its self-test log and summary error log (512 bytes each); an NVMe drive its SMART log page, then optionally its self-test log (564 bytes) and error log (1024 bytes). Self-tests started on such a file are pretended. `HSM_FREEZE_SIM="stall:3.5@4,suspend:3600@8"` reports a 3.5 s freeze 4 s after the start and an hour's suspend after 8 s, `HSM_STILL_AFTER=10` marks sensors as unchanged after 10 s instead of 5 minutes, and `HSM_RUNAWAY_MIN=2` calls out a busy process after 2 minutes instead of 15. A `net/route` file in `HSM_PROCFS` points the network check at any router, and `class/dmi/id` in `HSM_SYSFS` describes a machine. A USB device there is a folder in `bus/usb/devices` laid out as the kernel does it (`idVendor`, `speed`, `bos_descriptors`, a `port` link, interface folders), and `HSM_USB_IDS` points at a `usb.ids` for devices that don't name themselves; a screen is a `class/drm/card0-HDMI-A-1` folder (`status`, `enabled`, `modes`, `edid`); bytes 216-231 of an NVMe log page hold its heat slowdown counters. `HSM_WIFI_FAKE=dir` answers the Wi-Fi questions from `dir/wlan0.iface` and `dir/wlan0.station`, which hold the nl80211 messages the kernel would send. For NVIDIA code, a small library of your own named `libnvidia-ml.so.1` in `LD_LIBRARY_PATH` stands in for the driver's. These variables are ignored when the binary runs setuid or with file capabilities.

## Ideas for later

* Intel `xe` driver and ARM (Mali / Adreno devfreq) GPU load
* AMD CPU throttling reports
* the cooling check for graphics cards

## License

Copyright (C) 2026  Yormask

hsm is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

It is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty of merchantability or fitness for a particular purpose. See the [LICENSE](LICENSE) file for the full text.

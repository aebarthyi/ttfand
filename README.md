# ttfand — Tenstorrent fan daemon

A small C++ daemon that reads the **max ASIC temperature** across the chips on a
Tenstorrent card via **tt-umd** and drives a **Linux hwmon PWM fan** on a preset
curve. Starts at boot via systemd. Fails **safe** (full speed) on any error.

## Read this first — what it does and does not control

This daemon controls a **host-side PWM fan** (a motherboard/chassis fan header,
or a PWM fan you've attached to the card's heatsink and wired to a header),
keyed to the card's die temperature.

It does **not**, and on Wormhole **cannot**, control the card's *own* fan:

- Wormhole's ARC message set exposes no fan command (`GET_AICLK`,
  `ARC_GO_BUSY/IDLE`, `ARC_GET_HARVESTING`, reset/iATU/test, … — no fan).
  The `force_fan_speed` host request people cite is part of the **Blackhole**
  SMC message queue, not Wormhole.
- Wormhole's board-management firmware (which owns the on-card fan curve) is
  shipped as proprietary binaries; the open Zephyr firmware you'd edit-and-reflash
  is Blackhole-only.

So on an n300s the card's own fan runs Tenstorrent's baked-in curve regardless.
This daemon is the way to get a *preset curve you control* for a passive card:
put a controllable fan on the heatsink / in the airflow path and let this drive
it from the real die temperature.

If your fan is hardwired straight to the board (not to a host PWM header), it's
firmware-owned and out of reach — nothing host-side can help.

## Build

Needs `libtt-umd` (which needs tt-kmd installed and the device present).

Option A — UMD installed as a package (exports `umd::tt-umd`):

```bash
sudo apt install ./umd-dev-*.deb        # or `cmake --install` from a UMD build
cmake -B build -G Ninja
cmake --build build
```

Option B — point at an in-tree tt-umd build:

```bash
git clone https://github.com/tenstorrent/tt-umd
cmake -S tt-umd -B tt-umd/build -G Ninja && cmake --build tt-umd/build
cmake -B build -G Ninja -DTT_UMD_ROOT=$PWD/tt-umd
cmake --build build
```

## Install

```bash
sudo install -m0755 build/ttfand /usr/sbin/ttfand
sudo install -m0644 systemd/ttfand.service /lib/systemd/system/
sudo install -m0644 ttfand.conf.example /etc/ttfand.conf   # then edit it
sudo systemctl daemon-reload
sudo systemctl enable --now ttfand
journalctl -u ttfand -f
```

## Configure

Edit `/etc/ttfand.conf`. The one thing you must set is `pwm_path`. Find it:

```bash
for h in /sys/class/hwmon/hwmon*; do echo "$h -> $(cat $h/name 2>/dev/null)"; done
ls /sys/class/hwmon/hwmonN/pwm*
# test (1 = manual control):
echo 1   | sudo tee /sys/class/hwmon/hwmonN/pwm1_enable
echo 200 | sudo tee /sys/class/hwmon/hwmonN/pwm1
```

`sensors` and `pwmconfig` (lm-sensors) help map which `pwmN` drives which fan.

Reload the curve without a restart: `sudo systemctl reload ttfand` (SIGHUP).

## Safety behavior

- Full speed (`fail_pwm`, default 255) on startup before the first good reading,
  on `fail_grace` consecutive read failures, and on shutdown.
- `systemd Restart=on-failure` restarts on crash or a stale device handle after
  a card reset; the fan is at full speed across the gap.
- `min_pwm` keeps airflow on the passive card even at idle.
- Hysteresis (`hysteresis_c`) stops the fan hunting around a curve breakpoint.

## The two lines to check against your UMD version

tt-umd's telemetry API has moved across releases. If it doesn't compile, the two
spots flagged `ADJUST:` in `src/ttfand.cpp` are almost certainly why:

1. **Chip enumeration** — `Cluster::get_target_device_ids()`. Older trees used
   `get_all_chips()` / `get_target_mmio_device_ids()`.
2. **Temperature accessor** — `TTDevice::get_firmware_info_provider()->get_asic_temperature()`.
   If your headers differ, use the tag-based reader
   (`SmBusArcTelemetryReader::read_entry(wormhole::LegacyTelemetryTag::ASIC_TEMPERATURE)`)
   and convert the 16.16 fixed-point value with `>> 16` (or `/ 65536.0`).

Check `tt-smi -s` shows a sane Core Temp before trusting the daemon; that JSON is
also a stable cross-check if you ever want a UMD-free fallback reader.

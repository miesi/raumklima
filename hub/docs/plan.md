# Hub firmware - plan

Status 2026-11-26: M0 to M4 complete and tested on the CrowPanel 5.79 (see "Milestones"),
also intervals, Carbon, syslog and the crash handling. The hub is in `hub/` of the repository
`raumklima` since 2026-11-26. The system plan (nodes, roles, KNX) is in `../../docs/plan.md`.
Open from M4: visual check of the panel (orientation, seam), key test, NTP fallback test
(option 42 off).

## Decisions (2026-11-16)

- Repo `raumklima-hub`, GitHub account `miesi`, MIT license, English only. (2026-11-26: the
  repository is now `raumklima`, the hub is in `hub/`.)
- Graphics: LVGL 9, 1 bpp rendering, full-frame render (`LV_DISPLAY_RENDER_MODE_FULL`).
- Pages switched with the rotary switch: overview, network, one page per sensor, system.
- Display refresh once per minute; time without seconds, uptime in minutes.
- Time zone: CET/CEST (`CET-1CEST,M3.5.0,M10.5.0/3`, changed 2026-11-17 from UTC), shown as
  `%Z`; ISO offset on the time page.
- IPv6: link-local filtered out, at most 2 addresses shown, "+" if there are more.
- WLAN: WPA2/WPA3 personal only. Build-time defaults from the git-ignored `wlan.config`.
- BME680 with BSEC2 now; BSEC binaries fetched at build time, never committed.
- BME680/BME690 are never used together, same for SHT40/SHT45 (SHT45 if the budget allows).
  BME680, BME688 and BME690 are told apart by the variant ID register 0xF0.
- 2026-11-17: NTP fallback is the DNS name `ntp.1und1.de` instead of the address literals
  212.227.123.5 and 2001:8d8:fe:123::5 from the spec: a public firmware must not pin an
  operator's server address. Consequence: the fallback needs DNS from DHCP.

## Why

The room climate hub (see the spec "Raumklima-Sensorhub", 2026-11-06) needs one firmware for
several boards and sensor sets. The first board is the Elecrow CrowPanel 5.79" e-paper
(ESP32-S3 N8R8) with a BME680 and a BH1750. Later builds add SCD41, SHT40/SHT45, BME690 and
1-Wire temperature probes behind a DS2484. The skeleton must take those sensors without
restructuring, and every device-specific value must come from NVS, not from the build.

## What - scope of milestone 1 (this skeleton)

On the CrowPanel 5.79", show on the e-paper:

- date and time in UTC, the time zone string, NTP sync state and source
- BH1750: illuminance in lx
- BME680: temperature, relative humidity, pressure, gas resistance (raw, no IAQ yet)
- WLAN: connected yes/no, SSID, RSSI in dBm
- IPv4 address (DHCP, with hostname option 12)
- IPv6 address (SLAAC; global and link-local)
- uptime

Time: SNTP. NTP servers from DHCP option 42; without option 42 use `ntp.1und1.de`.
A server set on the console overrides both (as in the spec).

Configuration: serial console on UART0 (CH340C), values stored in NVS.

Out of scope for milestone 1: Carbon, syslog, OTA, BSEC/IAQ, charts, LED traffic light,
Bluetooth, power-fail handling. The structure below leaves room for each of them.

## How

### Layout of hub/

The layout as it is on 2026-11-26 (the first plan of 2026-11-16 had some other names):

```
hub/
  CMakeLists.txt              # project; espcoredump log level (see "Crashes and hangs")
  sdkconfig.defaults          # PSRAM, lwIP (DHCP NTP, IPv6), console, watchdogs, core dump
  partitions.csv              # OTA-ready, 1 MB coredump
  main/app_main.c             # init order only, no logic
  components/
    board/                    # pin map, power rails, keys, LED
    appcfg/                   # typed settings in NVS with defaults
    core/                     # app events, sched.h (interval boundaries)
    netmgr/                   # WLAN STA, DHCPv4 hostname, SLAAC, link state, DNS checks
    timesync/                 # SNTP, DHCP option 42, fallback server, sync state
    i2cbus/                   # two i2c_master buses, scan
    onewire/                  # 1-Wire ROM IDs, names in NVS
    sensors/                  # driver interface, registry, polling task
      drivers/                # bh1750 bme68x_bsec bme69x bsec_glue ds2484 scd4x sht4x
    bosch_bme68x/             # Bosch SensorAPI and BSEC2 (fetched) or BSEC3 (local)
    metrics/                  # last value and interval windows for each metric
    carbon/                   # Graphite plaintext sender, PSRAM buffer
    netlog/                   # syslog client (RFC 5424), PSRAM ring
    health/                   # supervisor, crash history, safe mode, core dump report
    coredumpd/                # HTTP port 80 with the core dump, only while there is one
    epd/                      # e-paper driver (SSD1683 x 2)
    ui/                       # LVGL pages, fonts
    console_cmds/             # esp_console commands
  docs/
  tools/                      # build.sh, mkfont.py
```

Rule: components talk through `esp_event` (custom event base) and `metrics`, not through each
other's internals. The UI never calls a sensor driver; it reads `metrics` and `netmgr`/`timesync`
status structs.

### Sensor driver interface (the part that must scale)

```c
// one instance per physical sensor found on a bus
typedef struct sensor_driver {
    const char    *model;          // "bme680", "sht45", "ds18b20"
    const uint8_t *i2c_addrs;      // candidate addresses for probing
    size_t         n_addrs;
    uint32_t       period_ms;      // poll period
    esp_err_t (*probe)(i2c_master_bus_handle_t bus, uint8_t addr, void **ctx);
    esp_err_t (*read)(void *ctx, metric_sink_t *sink);   // emits 0..n metrics
    void      (*deinit)(void *ctx);
} sensor_driver_t;
```

- The registry probes every known driver at boot (and on console `sensors rescan`).
- A driver can emit several metrics: the BME68x emits 4, the DS2484 emits one per DS18B20 ROM ID.
- Metric key = `<instance>.<quantity>_<unit>`, e.g. `bme680.temperature_c`, `bh1750.illuminance_lx`,
  `ds18b20_28ff4c1a.temperature_c`. Instance names default to the model and can be renamed on
  the console (`sensor name ...`, `onewire name ...` from the spec).
- When several sensors measure the same quantity, `metrics` publishes a primary value per
  quantity by priority: temperature/humidity SHT4x > BME6xx > SCD41, pressure BME6xx.
  The SCD41 gets pressure compensation from the BME6xx (spec: `set_ambient_pressure`).
- I2C errors are counted per device (self-monitoring metrics from the spec).

I2C addresses - conflicts decide which combinations are possible on one bus:

| Sensor | Address(es) | Status |
|---|---|---|
| BH1750 | 0x23 (ADDR low), 0x5C (ADDR high) | ASSUMPTION, from datasheet memory - verify |
| BME680 / BME690 | 0x76 or 0x77 (SDO pin) | ASSUMPTION, verify |
| SCD41 | 0x62 fixed | ASSUMPTION, verify |
| SHT40 / SHT45 | 0x44 for the -AD1B variants, 0x45 for -BD1B, 0x46 for -CD1B | ASSUMPTION, verify |
| DS2484 | 0x18 fixed | ASSUMPTION, verify |

Consequence: SHT40 and SHT45 together need different address variants, BME680 and BME690
together need one on 0x76 and one on 0x77. If that is not possible: second I2C bus (ESP32-S3
has two controllers).

### Limits and 1-Wire data model

- I2C: at most `CONFIG_HUB_SENSORS_MAX` (default 8) sensor instances. More found: error in the
  log with the ignored addresses, `WARNING` in `show status`, "! n sensor(s) over limit" on the
  overview; the sensors found first (registry order, then address order) keep working.
  Tested 2026-11-16 with the limit set to 1 (BME680 kept, BH1750 at 0x23 reported).
- Metrics: at most `CONFIG_HUB_METRICS_MAX` (default 64) keys. A new key in a full table takes
  the slot of the oldest stale metric (renamed or removed probe); without one it is dropped:
  error in the log, "metrics full" warning in `show status` and on the overview while it lasts.
  Tested 2026-11-17 with the limit set to 23 (= keys in use): renaming a 1-Wire probe dropped
  the new key (error, warning); 60 s later it replaced the stale old key and the warning cleared.
- DS2484 (fixed address 0x18): up to `OW_MAX_DEVICES` = 8 devices on its 1-Wire bus.
  Each device = 64-bit ROM ID (shown as 16 hex digits, family code first) plus an optional
  name set on the console (`onewire name <rom> <name>`, `[a-z0-9_]`, max 15 chars, saved in
  NVS at once, up to 16 names so spare probes keep theirs). The name is the metric instance:
  `<name>.temperature_c`; unnamed probes use `ow_<rom>`.
- 9 or more devices: the search keeps counting up to 32, then error "n 1-Wire devices found,
  max 8 - using the first 8" (log, console, display); the first 8 in search order are used.
  Search order is deterministic (ROM bit order), so the same probes win after every reboot.
- `onewire selftest [n]` runs the search on a simulated bus with n devices and checks CRC-8
  against the Maxim AN27 example ROM. Tested 2026-11-16 with n = 0, 3, 8, 9, 40: PASS.
- DS2484 driver: command set from the Linux ds2482 driver (Analog Devices datasheets were not
  reachable). Tested 2026-11-16 with 2 DS18B20: search, conversion, CRC ok.
- SHT4x and SCD4x drivers follow Sensirion's embedded-i2c-sht4x / -scd4x (BSD-3) command
  tables; BME690 uses Bosch's BME690_SensorAPI. All tested 2026-11-16 on the CrowPanel.
  BME690 IAQ with BSEC3 3.3.0.1 from third_party/bsec3 (git-ignored), tested 2026-11-17.
  BSEC2 and BSEC3 export the same symbols: one per build; BSEC3 if present, else BSEC2.
- SCD41: pressure compensation, offset auto against SHT4x (tested: 4.00 -> 4.89 C, then
  reset), FRC with 3 min / 40 ppm stability check (refusal tested; a real FRC was NOT run
  indoors on purpose), fresh-air session on BOOT long press, ASC target = outdoor value.
  FRC plausibility (measured mean within 150 ppm of the target unless `force`) and the session
  window tested 2026-11-17 indoors at about 1450 ppm: `scd41 frc 430` was refused by the
  plausibility check at 65 s uptime; a session started with a full window restarted at 1/36
  readings and, stable after 3 min, was refused (1443 ppm measured, target 430). No FRC ran.

### SCD41 incident 2026-11-17

- Fresh-air FRC 2026-11-16 23:52 UTC, 430 ppm, correction +31 (plausible: indoor at an open
  window read 396 ppm). Next morning: CO2 0 ppm with valid CRC, T/RH plausible; self test
  0x0004 at 06:50 UTC (also after a reboot, which does not cut the sensor supply).
- About 07:20 UTC it read ~1450 ppm again; 08:xx: self test 0x0000, periodic 1226 ppm,
  single shot 1244 ppm. What happened in between is not known (power cycle by the user?).
- Sensirion (testing guide 2021-01, p. 15): a self test result other than 0 means a sensor
  malfunction OR an unstable or insufficient supply. No public decoding of 0x0004 found.
- Candidates, none proven: condensation (cold outdoor air, then a warm room; datasheet: non-
  condensing only), supply (peak 175/205 mA at 3.3 V, <= 30 mV variation from other loads,
  separate LDO recommended - here shared 3.3 V with ESP32 WLAN and e-paper via a STEMMA hub).
- Firmware consequences: readings < 300 ppm are a fault, not data; debug commands in 'scd41'.
- Later the same day (after Thomas' power cycle ~07:00): good for about 1 h, then again mostly
  0 ppm with single plausible readings (~537 ppm), self test 0x0004. WLAN radio off for 2 min:
  still only zeros (18 / 24 / 18 readings on/off/on) - WLAN load is not the (only) cause.
  power_down/wake_up does not recover it; only cutting the supply did, for about an hour.
- Health check (changed 2026-11-17 evening: only on implausible readings, at most every 30 min;
  first: every 30 min regardless): self test,
  on failure power_down/wake_up + second self test, logged ('scd41 health'), counters in NVS.
  Runs as a state machine, so the BME690/BSEC 3 s cadence is not disturbed (BSEC needs
  +-6.25 %, Integration Guide 3.2). Conflict: Sensirion's ASC assumes at least 4 h of
  uninterrupted measurement at a time (datasheet 3.7); a check every 30 min stops that.
- The self test is not a reliable indicator: 2026-11-17 20:46 it returned 0x0000 while the
  sensor was giving 0 ppm readings in between plausible ones.
- Isolation tests 2026-11-17 evening (SCD41 alone on bus 2, other sensors and hub on bus 1):
  a) 3.3 V from the header: CO2 0 ppm from the first reading on (11 of 11 in 60 s);
  b) own supply, 2 alkaline cells 2.74 V, common GND: 23 of 23 readings 0 ppm in 120 s,
     self test 0x0004, single shot 0 ppm; T and RH plausible in all cases.
  Correction (same evening): Thomas' multimeter showed the cells dropping 2.74 -> 2.70 V every
  few seconds, i.e. with each measurement. A multimeter averages; the real dip during the
  175-205 mA peak is larger, likely above Sensirion's 30 mV limit. So test b) does not separate
  "supply" from "sensor". Ruled out: hub, wiring, the other sensors. Open: supply vs. sensor.
  Next: a clean 3.3 V from a separate LDO (with its own input), common GND. PIM587 has its own I2C pull-ups to its supply (bus 2 idle 1/1).
  Note: with 3 cells (4.23 V) and common GND the pull-ups would put SDA/SCL above the ESP32-S3
  GPIO limit - only supplies <= 3.3 V for a sensor on these pins.
- Second I2C bus (IO15/IO16, header pins 3 and 18) to run the SCD41 apart from the other
  sensors and the hub. Sensors are probed on both buses; duplicate instance names get "_2";
  SCD4x and DS2484 are single-instance (a second one is reported, not driven).
- Pull-up check at boot: gpio_reset_pin() enables the internal pull-up, so an open pin read 1.
  Now the line is discharged first; tested: bus 1 = 1/1, open bus 2 = 0/0.
- Supply measurement: the ESP32-S3 has no internal supply channel (soc_caps); GPIO9 on the
  header is ADC1 channel 8, continuous ADC up to 83.3 kHz, 12 bit. Needs a divider.

### Networking

- `esp_netif` default WLAN STA. Hostname from NVS (`set hostname`) via
  `esp_netif_set_hostname()` before DHCP starts, so it goes into option 12.
- IPv6: `esp_netif_create_ip6_linklocal()` after association, global address via SLAAC
  (`IP_EVENT_GOT_IP6`). DHCPv6 off. ASSUMPTION: lwIP forms the SLAAC address from EUI-64 (no
  privacy extensions), so the address is stable - check on the device.
- DNS servers from DHCP. RSSI via `esp_wifi_sta_get_ap_info()`, refreshed every 10 s.
- Reconnect with backoff; count reconnects (self-monitoring).

### Time

- `esp_netif_sntp` with `CONFIG_LWIP_DHCP_GET_NTP_SRV=y`. Fallback `ntp.1und1.de` as the configured
  entry; DHCP option 42 replaces it when present.
  The exact replace semantics of lwIP must be tested with and without option 42 (test 3 below).
- `TZ` from NVS, default `CET-1CEST,M3.5.0,M10.5.0/3`. Verified with `show tz` 2026-11-17:
  switches 2027-03-28T01:00Z and 2027-10-31T01:00Z (last Sundays, EU rule). `set tz` only calls `tzset()`;
  SNTP keeps its servers. Tested 2026-11-17: after `set tz CET-1CEST,M3.5.0,M10.5.0/3` the
  source stayed DHCP (192.168.178.1), the clock showed CET.
- Server names from the console live in static storage: lwIP keeps only the pointer and
  resolves the name on every request.
- The UI shows "not synced" until the first sync; no fake 1970 time on screen.

### Derived values

- Dew point from the SHT4x (Magnus, Sonntag 1990: a = 17.62, b = 243.12 C) as
  `<sht>.dewpoint_c`; warning when any temperature metric is within 1 K of it. Tested with
  the margin set to 12 K: the colder DS18B20 (21.9 C, dew point 14.2 C) was reported.
- DNS watch: names used for NTP (later Carbon) resolved every 5 min and after new addresses.

### Display

- 1-bit framebuffer 800 x 272 in PSRAM (27.2 kB), SPI2 at IO11/IO12, CS/DC/RST/BUSY 45/46/47/48,
  panel power IO7.
- Refresh policy: partial refresh once per minute (time has minute resolution on screen), a
  full refresh every N partial refreshes against ghosting (N configurable, start at 30),
  immediate partial refresh on WLAN connect/disconnect and NTP sync.
- Fonts: see open question about LVGL vs. own minimal renderer.

### Partition table (8 MB flash)

| Name | Type | Size | Why |
|---|---|---|---|
| nvs | data/nvs | 24 KB | settings, BSEC state later |
| otadata | data/ota | 8 KB | OTA from v2 on, no repartition later |
| phy_init | data/phy | 4 KB | |
| ota_0 | app | 3 MB | |
| ota_1 | app | 3 MB | |
| coredump | data/coredump | 1 MB | crash dump incl. DRAM and used PSRAM heap (~0.4 MB measured) |
| storage | data/littlefs | 896 KB | unused - buffers stay in RAM (flash wear) |

Until OTA exists, `ota_0` is flashed over USB like a factory partition.

### Console (milestone 1 subset)

| Command | Effect |
|---|---|
| `help` | list commands |
| `show status` | IPs, SSID, RSSI, DHCP lease, NTP state/server/offset, sensors, uptime, reset reason, heap |
| `show config` | NVS settings, WLAN key masked |
| `set wifi <ssid> <psk>` | WLAN credentials - missing from the spec, needed for v1 |
| `set hostname <name>` | |
| `set tz <posix>` | default `CET-1CEST,M3.5.0,M10.5.0/3` |
| `set ntp <server> [<server>]` / `set ntp dhcp` | override / back to DHCP + fallback |
| `i2c scan` | addresses on the bus |
| `sensors` / `sensors rescan` | detected sensors and last values |
| `save` / `reboot` / `factory-reset` | |

The command names follow the spec so later commands (`set carbon`, `onewire scan`, `scd41 frc`)
fit in.

### Milestones and tests

Each milestone ends with a test on the device that a third person can repeat.

1. **M0 - builds and boots.** `idf.py build flash monitor` works, boot log shows octal PSRAM
   8 MB, console prompt, `show config` prints defaults.
   Test: cold start, `help`, `factory-reset`, reboot.
2. **M1 - network.** `set wifi`, DHCP with hostname, SLAAC.
   Test: hostname visible in the FritzBox lease list; `ping` IPv4 and IPv6 from the Mac;
   `show status` addresses match the router.
3. **M2 - time.** Test A: DHCP without option 42 -> server `ntp.1und1.de`
   in `show status`. Test B: with option 42 -> that server. Test C: `set ntp` overrides both.
   Clock compared with `date -u` on the Mac, deviation < 1 s.
4. **M3 - sensors.** `i2c scan` shows BH1750 and BME680; `sensors` shows plausible values
   (lux changes when covering the sensor; BME680 pressure within a few hPa of a weather station).
   Pull a sensor during operation: error counter rises, no crash, value marked stale.
5. **M4 - e-paper.** Full refresh, then partial refresh; status screen with all fields above.
   Test: one hour of operation, no ghosting visible, time correct at minute boundary.
6. **M5 - robustness.** Task watchdog, reset reason and uptime on screen and console, WLAN
   off/on at the router recovers without reboot.

### Intervals, Carbon and syslog (2026-11-18)

Decisions (Thomas):
- Sensors with a mandatory cadence keep it: BSEC 3 s (VERIFIED: config `*_33v_3s_4d`, Integration
  Guide: +-6.25 %), SCD41 periodic 5 s (ASC, FRC and T offset depend on the mode). Explained in
  `set interval`.
- No own sensor interval: sensors without a cadence are read once per min(display, Carbon), in
  one cycle that ends 100 ms before the boundary. Lead = longest of the last 8 measured cycles
  (the first cycle after boot, with the 1-Wire search, does not count) + 100 ms.
- Display and Carbon interval 10..3600 s, default 60/60, aligned to UTC multiples. An interval
  shorter than the read cycle is refused; one not a multiple of the other gets a friendly note.
- Display shows the mean of the interval; Carbon gets mean plus min/max where an interval held
  more than one reading. That is always the case for the cadence sensors, for the others only
  if display < Carbon.
- Syslog lines: subsystem (ESP_LOG tag) as MSGID and at the start of the message, then
  key=value, like the queue ID in Postfix logs.
- First step plain text (Carbon TCP, syslog UDP/TCP).

Test receivers: go-carbon (VERIFIED: commit 54407c0a, built from ../go-carbon) and Homebrew
rsyslog 8.2610.0, both as user processes, `tools/receivers/`.

TLS / client certificates (for a later step):
- Syslog: possible. RFC 5425; rsyslog's gtls driver (in the Homebrew build; ossl is not)
  supports `StreamDriver.AuthMode="x509/name"` with `PermittedPeer` - VERIFIED in a live test:
  no client certificate and a wrong name were refused, the right one accepted. With TLS 1.3 the
  refusal comes after the handshake, so the client cannot use "handshake ok" as success.
- Carbon: go-carbon's plaintext TCP receiver has no TLS at all (VERIFIED: `receiver/tcp/tcp.go`,
  `net.ListenTCP`). Needs a TLS terminator with client certificate check in front (stunnel,
  HAProxy - ASSUMPTION, not tested) or the WireGuard tunnel from the spec.
- ESP side: esp-tls takes a client certificate and key (VERIFIED: `clientcert_buf`,
  `clientkey_buf` in `esp_tls.h`). The key would sit readable in flash/NVS: protecting it
  (flash/NVS encryption or the DS peripheral) needs eFuses, which this project never burns.
  Anyone with the device and a USB cable can read the key - the certificate identifies the
  device, it does not prove that nobody copied it.

Buffers: RAM only (Thomas, 2026-11-19): may be lost on reboot, flash cells have few write
cycles. Receivers down, VERIFIED on the device 2026-11-19: Carbon buffered 32 batches
(116 KB, "Connection reset by peer" once a minute) and sent all 33 in one go when go-carbon
came back, with the original timestamps. Syslog over UDP reported `sent 100, errors 0` while
rsyslog was down - the lines were lost; buffering needs TCP (planned together with the client
certificate).

Open: in that run the `hub.*` self metrics are missing for 21:49 UTC while the sensor values of
the same batch are there - `publish_self()` did not run before that window was closed. Cause
not found yet (ASSUMPTION: the sensor tick and the Carbon boundary of the carbon task fell
apart once).

### Crashes and hangs (2026-11-19)

Decisions (Thomas): a hung task is aborted at once after 2 x interval + 30 s; core dump with
DRAM; syslog warning with a summary after the reboot; safe mode after 3 crashes in 10 minutes;
the crash test commands stay in the firmware.

Hardware (VERIFIED, IDF v6.1.1 sources): two timer group watchdogs (interrupt and task
watchdog), the RTC watchdog (own clock; also guards the panic handler, 10 s,
`esp_system/port/panic_handler.c`) and the analog super watchdog - fed by setting
`RTC_CNTL_SWD_FEED` (bit 29 of `RTC_CNTL_SWD_CONF_REG`), but the bootloader turns on its auto
feed (`bootloader_esp32s3.c`), so it is no app watchdog. The watchdog hardware does not dump
memory; the panic handler that its first stage starts writes the core dump. A final stage
hardware reset, brownout and power loss leave no dump, only the reset reason.

Design:
- `health_register()` / `health_alive(id, interval)` in each task loop; the supervisor (priority
  10, itself on the task watchdog) checks once a second and calls `esp_system_abort("stall:
  <task>")`. Not watched: console and keys (they block on input by design).
- RTC_NOINIT state with a check word: boots, crashes, run time of the last 3 crashed runs, the
  task of a supervisor abort. Crash = reset reason panic, interrupt/task/other watchdog, CPU
  lockup. Any other reset clears the history (also esptool's reset when reading the dump).
- Safe mode: `sensors_start()` creates its locks but probes nothing and starts no task; the
  display is not started (the e-paper keeps its last picture).

Found on the device (VERIFIED, each by a `crash` test):
- `CAPTURE_DRAM` copies every used heap block, PSRAM included: 2.6 MB with the Carbon buffer on
  the heap, "Not enough space". Fix: big buffers static in PSRAM (`EXT_RAM_BSS_ATTR`), not in the
  dump; dump 0.36 MB, partition 1 MB. The IDF doc says PSRAM data is not dumped - true for
  `.ext_ram.bss`, not for the heap.
- Debug log level made the dump code print ~800 lines at 115200 baud; the 10 s RTC watchdog of
  the panic handler cut the dump (bad CRC). Fix: espcoredump compiled with info level
  (`CMakeLists.txt`). Dump now written in 0.85 s.
- Core dump stack: 280 of 1792 bytes left; now 2560.
- A busy loop in an unpinned task of low priority (console, 2) did not trigger the task
  watchdog in minutes (ASSUMPTION: the scheduler moves it between the cores, both idle tasks
  still run). The test `spin` therefore runs pinned to core 1 (watchdog after 10 s); watched
  tasks are caught by the supervisor either way.
- Interrupt watchdog panic: no dump - the heap walk of `CAPTURE_DRAM` takes the heap lock, the
  pending interrupt fires again ("Re-entered core dump!", backtrace in `heap_caps_walk`). 3000 ms
  instead of 800 ms did not help; left at the default.
- `esp_core_dump_get_summary()` gives a garbage PC and a corrupted backtrace after an abort or a
  watchdog; the syslog line leaves them out then. `esp-coredump info_corefile` shows the full
  stacks (stall test: `sensors_task` - `poll_one` - `xQueueSemaphoreTake` on the bus lock).

Open: fetching the dump over the network (HTTP on the device, or mail) - to be decided.

### Core dump over HTTP and safe mode screen (Thomas, 2026-11-26)

- `coredumpd` starts the HTTP server on port 80 only while a valid core dump is in flash.
  `GET /coredump?token=<token>` returns the raw partition data. The token: 16 hex characters
  (64 bits) from the hardware RNG before the radio starts (`bootloader_random_enable()`), new
  for each boot. It is in the syslog line `event=coredump_http` (a separate line: the summary
  line is near the 216 byte limit of netlog), in `coredump` and on the system page.
  `coredump erase` posts `APP_EVENT_COREDUMP_ERASED`, the server stops.
- Safe mode: the normal UI task does not start. One task waits up to 30 s for IPv4, shows one
  static screen and ends. If the display code causes the crashes, it gets one chance to crash,
  not one each minute.

Test 2026-11-26 on the CrowPanel (VERIFIED):
- No dump: `curl` to port 80: "Couldn't connect to server".
- After `crash panic`: no token or a wrong token 403, other path 404, the right token 200 with
  365664 bytes in 0.63 s over IPv4 and over IPv6 (same SHA-1). `esp-coredump info_corefile`
  shows the stack of the crash test. The token of the previous boot gets 403.
- `coredump erase`: `event=stopped`, port 80 closed again.
- 3 crashes in 122 s of run time: safe mode, screen after 8 to 10 s, download works. `screen 3`:
  "SAFE MODE" and five text lines, the URL ends at x = 654 of 792.
- Found: `font_dejavu_bold_72` has digits only (`tools/mkfont.py`, `DIGITS_ONLY`); the title
  uses `font_dejavu_bold_32`.

### Next hub tasks

- KNX (send on the primary hub, listen on all hubs), roles, receiver for nodes: see
  `../../docs/plan.md`.

### Later, in this structure

- Carbon/syslog over TLS with client certificates (syslog then over TCP, so it buffers while
  the receiver is down).
- OTA over HTTPS, signed images, rollback.
- Board files for XIAO ESP32-S3 / QT Py ESP32-S3 (native USB console instead of UART0).

Done since the first plan: SCD41 with FRC and temperature offset, SHT4x, DS2484 with DS18B20,
BME690 with BSEC3, intervals, Carbon, syslog, crash handling.

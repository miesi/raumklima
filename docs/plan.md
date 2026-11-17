# Firmware skeleton - plan

Status: 2026-11-16 - M0 to M4 implemented and tested on the CrowPanel 5.79 (see "Milestones").
Open: visual check of the panel (orientation, seam), key test, NTP fallback test (option 42 off).

## Decisions (2026-11-16)

- Repo `raumklima-hub`, GitHub account `miesi`, MIT license, English only.
- Graphics: LVGL 9, 1 bpp rendering, full-frame render (`LV_DISPLAY_RENDER_MODE_FULL`).
- Pages switched with the rotary switch: overview, network, one page per sensor, system.
- Display refresh once per minute; time without seconds, uptime in minutes.
- Time zone shown as "UTC" (strftime `%Z` of the POSIX TZ `UTC0`).
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

### Repo layout

```
raumklima-hub/
  CMakeLists.txt
  sdkconfig.defaults            # common: PSRAM, lwIP (DHCP NTP, IPv6), console, watchdog
  sdkconfig.defaults.esp32s3
  partitions.csv                # OTA-ready from day 1 (see below)
  main/
    app_main.c                  # init order only, no logic
    Kconfig.projbuild           # board choice, compile-time defaults
  components/
    board/                      # pin map + power rails + buttons, one file per board
    appcfg/                     # typed settings in NVS with defaults, change notifications
    netmgr/                     # WLAN STA, DHCPv4 hostname, SLAAC, link state, RSSI
    timesync/                   # SNTP, DHCP option 42, fallback servers, sync state
    i2cbus/                     # one i2c_master bus, scan, error counters per device
    sensors/                    # driver interface, registry, polling task
      drivers/bh1750.c
      drivers/bme68x_bsec.c     # BME680 + BSEC2, BME688 raw, BME690 detected only
      (later) drivers/scd4x.c sht4x.c ds2484.c bme69x.c
    bosch_bme68x/               # fetches Bosch SensorAPI + BSEC2 at configure time
    metrics/                    # latest value per metric + 24 h ring buffer in PSRAM
    epd_ssd1683x2/              # e-paper driver, written from the SSD1683 datasheet
    ui/                         # screen layout, refresh policy
    console_cmds/               # esp_console commands
  docs/
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
- `TZ` from NVS, default `UTC0` (the POSIX string for UTC).
- The UI shows "not synced" until the first sync; no fake 1970 time on screen.

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
| coredump | data/coredump | 64 KB | crash dump, sent on next boot (spec) |
| storage | data/littlefs | rest (~1.9 MB) | Carbon buffering in v2 |

Until OTA exists, `ota_0` is flashed over USB like a factory partition.

### Console (milestone 1 subset)

| Command | Effect |
|---|---|
| `help` | list commands |
| `show status` | IPs, SSID, RSSI, DHCP lease, NTP state/server/offset, sensors, uptime, reset reason, heap |
| `show config` | NVS settings, WLAN key masked |
| `set wifi <ssid> <psk>` | WLAN credentials - missing from the spec, needed for v1 |
| `set hostname <name>` | |
| `set tz <posix>` | default `UTC0` |
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

### Later, in this structure

- Carbon sender reads `metrics`, own task, buffer in `storage` (spec v1/v2).
- Syslog RFC 5424 (UDP/TCP/TLS) after first NTP sync, buffer before.
- SCD41 (Sensirion driver, BSD-3), FRC, temperature offset vs. SHT4x.
- SHT40/SHT45 one driver `sht4x`, model from serial/ID read.
- DS2484 + DS18B20: driver emits one metric per ROM ID, `onewire scan/name`.
- BME690 + BSEC 3.x for IAQ; BSEC is a closed binary under Bosch license - not committed,
  fetched at build time (see open questions).
- OTA over HTTPS, signed images, rollback.
- Board files for XIAO ESP32-S3 / QT Py ESP32-S3 (native USB console instead of UART0).

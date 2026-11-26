# raumklima hub

Part of [raumklima](../README.md). ESP-IDF firmware for a room climate sensor hub: ESP32-S3, I2C sensors, e-paper status
display, network time, serial console. One firmware for all boards and sensor sets; every
device-specific value is set on the console and stored in NVS.

Status: early, runs on the CrowPanel 5.79. See [docs/plan.md](docs/plan.md).

## Hardware (first target)

- Elecrow CrowPanel ESP32 E-Paper HMI 5.79" (DIS08792E): ESP32-S3, 8 MB flash, 8 MB PSRAM,
  792 x 272 b/w e-paper (2x SSD1683), USB-C via CH340C to UART0.
  Pin map and sources: [docs/hardware-crowpanel-579.md](docs/hardware-crowpanel-579.md)
- Sensors on I2C bus 1 (SDA = GPIO3, SCL = GPIO8, header U5), all tested on the CrowPanel.
  Bus 2 (SDA = GPIO15, SCL = GPIO16) is scanned too; any sensor can sit on either bus.
  - SHT4x (SHT45) - reference temperature and humidity (preferred source for both)
  - SCD4x (SCD41) - CO2, with pressure compensation from the BME6xx
  - BME690 - temperature, humidity, pressure, gas resistance, IAQ with BSEC3
  - BME680 - as above, IAQ with BSEC2 or BSEC3
  - BH1750 - illuminance
  - DS2484 I2C-to-1-Wire bridge with up to 8 DS18B20 probes

## Bosch BSEC (IAQ)

BSEC is a closed binary under Bosch's license and not part of this repo.

- Default: BSEC2 is downloaded at build time (pinned, SHA256-checked). IAQ for the BME680 only.
- BSEC3 (IAQ for BME680 and BME690): download BSEC 3.x from Bosch Sensortec (click-through
  license) and unpack it to `hub/third_party/bsec3/` (git-ignored), so that
  `hub/third_party/bsec3/release_bin/IAQ/bin/esp/esp32_s3/libalgobsec.a` exists. Then
  `idf.py reconfigure`. The build log says which BSEC it uses. Tested with BSEC 3.3.0.1.
- `HUB_FORCE_BSEC2=1 idf.py -B build-bsec2 build` builds the BSEC2 variant anyway.

Building means accepting Bosch's license terms for the BSEC version used.

## Build

Requires ESP-IDF v6.1.x. Run all commands in `hub/`.

```sh
cd hub
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-10 flash monitor
```

On macOS the CH340C shows up as `/dev/cu.usbserial-*`. Exit the monitor with `Ctrl-]`.

## WLAN at build time (optional)

Create `wlan.config` in `hub/` (git-ignored) and run `idf.py reconfigure`:

```
ssid=<ssid>
psk=<pre-shared key>
```

Settings made on the console take precedence.

## First start

Connect to the console (same USB cable) and set at least:

```
set wifi <ssid> <psk>
set hostname <name>
save
reboot
```

`help` lists all commands, `show status` shows network, time and sensor state,
`screen` prints the e-paper content as ASCII art.

1-Wire probes behind a DS2484: `onewire scan`, then `onewire name <rom> <name>`.

SCD41 (CO2):

- `scd41 offset auto` - temperature offset against the SHT4x; run it after the device has been
  running for at least 30 min in steady conditions. `scd41 offset reset` undoes it.
- `scd41 frc <ppm>` - forced recalibration; refused unless the sensor ran 3 min, CO2 was
  stable (span <= 40 ppm over 3 min) and the measured mean is within 150 ppm of `<ppm>`.
  `scd41 frc <ppm> force` skips these checks. Outdoors use the fresh-air function instead:
- long press BOOT (or `scd41 fresh-air`): counts only readings taken after the press, waits
  up to 30 min until they were stable for 3 min, then calibrates to `set co2 outdoor <ppm>`
  (default 430) - or refuses if the measured mean is more than 150 ppm away from it.
  Progress and result on the display.
- The display warns when CO2 was not below 450 ppm for more than 156 h.
- Health check only on implausible readings, at most every 30 min (`scd41 health <min>`,
  0 = off): self test, on failure power_down/wake_up and a second self test; `scd41 health`
  shows the log. A healthy sensor is never interrupted (ASC needs 4 h without interruption).
- Debugging: `scd41 status`, `scd41 diag` (settings + built-in self test), `scd41 single`,
  `scd41 log on|off`, `scd41 reinit`, `scd41 sleep-cycle`, `scd41 asc on|off`.
- `scd41 factory-reset yes` erases every calibration of the sensor - irreversible.
- Readings below 300 ppm are treated as a sensor fault (warning on the display), not as data.
- Do not take the device from cold outdoor air straight into a warm room while it runs: the
  SCD4x is specified for non-condensing humidity only (datasheet v1.5, table 5).

BME6xx page: IAQ 0..500, lower is better (50 typical good air, 200 typical polluted air,
relative to the last ~4 days). Accuracy 0..3: 0 run-in, 3 calibrated (Bosch BSEC Integration
Guide). BSEC saves its learning state every 4 h and on `reboot`; a reset or reflash in between
loses up to 4 h of learning.

`led on|off` switches the red LED (off after boot). `wifi off|on` switches the radio (debug).

On an SCD41 fault the CO2 tile shows "--", not the BSEC estimate. Only without any SCD41 the
tile shows the BSEC estimate, captioned "CO2 eq. (BSEC est.)".

The SCD41 needs an SHT4x (reference for its temperature offset) and a BME6xx (pressure
compensation); without them the overview warns "missing SHT4x, BME6xx". Its own temperature and
humidity are measured inside its housing and appear only on its detail page, never on the
overview tiles.

Warnings on the overview: condensation risk when any measured temperature is within 1 K of the
dew point computed from the SHT4x; sensor limits; SCD41 faults and missing companions;
ventilation hint.
The network page shows whether the configured host names (NTP, Carbon, syslog) resolve.

## Intervals, Carbon and syslog

`set interval` (without arguments) explains the timing on the device.

- **Display interval** (default 60 s, 10..3600): the panel refreshes at every boundary, 20 ms
  after it - with 60 s at the start of every minute, aligned to UTC once NTP synced. Tiles show
  the mean of the readings in the interval. Keys still redraw at once.
- **Carbon interval** (default 60 s, 10..3600): at every boundary one batch is buffered and sent
  (Graphite plaintext over TCP), time stamp = the boundary in UTC.
- **Sensors with a cadence of their own** keep it: BME680/BME690 every 3 s (BSEC LP mode,
  +-6.25 %), SCD41 every 5 s (periodic mode; ASC, the fresh-air FRC and the temperature offset
  assume it). **All other sensors** (SHT4x, BH1750, DS2484 with its DS18B20) are read once per
  sensor interval = the shorter of the two intervals, in one cycle that ends 100 ms before the
  boundary. Its duration is measured (`show status`); an interval shorter than the read cycle
  is refused. One interval not a multiple of the other works, but gives a note: it is not
  recommended.

Carbon paths: `<prefix>.<hostname>.<instance>.<quantity>` = mean over the Carbon interval;
`..._min` and `..._max` only if the interval held more than one reading. With the defaults
(60/60) that means: min/max for BME and SCD41 (own cadence), none for SHT4x (and its dew
point), BH1750 and 1-Wire; with display 30 / Carbon 60 those get min/max too. States
(IAQ accuracy, BSEC status, the SCD41 pressure setting, counters) send their last value. The
window from boot to the first boundary is not sent (partial). Self monitoring under
`<prefix>.<hostname>.hub.*`: uptime, reset reason, free heap and PSRAM, RSSI, reconnects, NTP
syncs and offset, send errors and buffer fill of Carbon and syslog, read errors and read time
per sensor. Batches are buffered in PSRAM (2 MB) while the receiver is unreachable and before
the first NTP sync, then sent with their original time stamps; a reboot loses the buffer.

Syslog: RFC 5424 over UDP (RFC 5426) or TCP with octet counting (RFC 6587), facility local0.
Every ESP_LOG line goes there, with its subsystem (a component such as `timesync`, `carbon`,
or a sensor instance such as `sht4x`, `scd41`) as MSGID and at the start of the message, then
key=value pairs:

```
<132>1 2026-11-18T21:40:00.012Z hub-e1a418 raumklima-hub - sht4x - sht4x: event=read_failed model=SHT4x bus=1 addr=0x44 err=ESP_ERR_INVALID_CRC errors=1 streak=1
```

Lines are buffered from boot on (512 lines, PSRAM) and sent once the clock is set.

```
set carbon <host> [port] [prefix]      default port 2003, prefix raumklima; "set carbon off"
set syslog <host> [port] [udp|tcp]     default port 514 udp; "set syslog off"
set syslog level error|warning|info|debug
set prefer v6|v4                       address family tried first (default IPv6)
save
```

`log on|off` only changes the console. Plain text only for now; TLS (with client certificates)
is possible for syslog, not directly for go-carbon - see `docs/plan.md`.

Local test receivers on a Mac: `../tools/receivers/start.sh` (go-carbon on 2003, HTTP API on
127.0.0.1:8080; rsyslogd on 1516 UDP/TCP), data in `../.receivers/` (git-ignored),
`../tools/receivers/stop.sh`. go-carbon is built from a checkout next to the repository
(`../../go-carbon` from `hub/`).

Buffers are RAM only (PSRAM, lost on reboot - no flash wear): Carbon 2 MB, about 9 hours at
the defaults; syslog 512 lines. Syslog over UDP cannot buffer: a send to a host that is down
does not fail, the line is gone. Use TCP if lines must survive a receiver outage.

## Crashes and hangs

- Watchdogs: interrupt watchdog (800 ms), task watchdog (10 s, idle tasks), both with panic and
  core dump. A supervisor task (`components/health`) watches the long-running tasks (sensors,
  display, Carbon, syslog, DNS check): a task that does not come back within 2 x its interval
  + 30 s counts as hung, the supervisor aborts with a core dump of all task stacks.
- Core dump: ELF in the `coredump` partition, including `.data`, `.bss` and the heap. After the
  reboot a syslog warning `event=coredump_found` carries a summary; the dump stays in flash
  until `coredump erase` (the first dump of a series is kept, later ones do not overwrite it).
- Fetch over the network: while a core dump is in flash, the hub opens HTTP port 80. Without a
  dump the port is closed. The token is random for each boot. It is in the syslog line
  `event=coredump_http`, in the output of `coredump` and on the system page.
  ```
  curl -o core.bin 'http://<hub address>/coredump?token=<token>'
  esp-coredump info_corefile -c core.bin -t raw build/raumklima-hub.elf
  coredump erase                      (on the console: erases the dump, closes port 80)
  ```
  A wrong or missing token gets HTTP 403. `coredump` shows the URLs with the IPv4 and IPv6
  addresses (the hub has no mDNS, its host name can be unknown in the LAN).
- Fetch over USB:
  ```
  python -m esptool -p /dev/cu.usbserial-10 -b 230400 read-flash 0x620000 <size> core.bin
  ```
  `<size>` is `size=` from the syslog line or `coredump`. `idf.py coredump-info` works too, but
  reads at 460800 baud, which failed on the CH340 here. esptool resets the chip; that clears
  the crash history (not the dump).
- Safe mode: 3 crashes within 10 minutes of run time: the next boot starts without sensors
  (network, console, syslog, Carbon and the core dump download only). The display shows one
  static screen: "SAFE MODE", why the values are not updated, how to leave it and the core
  dump URL. `reboot` leaves safe mode. The crash history lives in RTC memory and is lost on
  power-off.
- Known gap: after an interrupt watchdog panic no core dump is written (IDF v6.1.1, see
  `sdkconfig.defaults`); reset reason and crash history are still there.
- Tests: `crash panic|fault|spin|irqoff|stall` on the console (crashes or hangs the hub on
  purpose). `show status` shows crashes, safe mode and the watched tasks.

## Display and keys

Rotary switch up/down: pages (overview, network, time, one per sensor, system).
MENU or EXIT: back to the overview. Rotary press: full refresh (clears ghosting).
Long press BOOT: fresh-air calibration of the SCD41.
The overview shows the sensor values and up to 8 1-Wire probes; if WLAN or IPv4 is missing
for more than 20 s, a "NETWORK DOWN" overlay appears.

## Network and time defaults

- IPv4 via DHCP (hostname sent as option 12), IPv6 via SLAAC.
- NTP servers from DHCP option 42; without it `ntp.1und1.de` (needs DNS from DHCP).
  `set ntp` overrides both.
- Time zone `CET-1CEST,M3.5.0,M10.5.0/3` (CET/CEST, EU summer time rule). `show tz` prints the
  next switches as the device computes them. A changed rule is a console command
  (`set tz <posix>`, `save`), not a firmware update.

## License

MIT, see [LICENSE](../LICENSE). Generated fonts: DejaVu (Bitstream Vera license),
see `components/ui/fonts/LICENSE-DejaVu`.

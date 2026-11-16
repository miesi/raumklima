# raumklima-hub

ESP-IDF firmware for a room climate sensor hub: ESP32-S3, I2C sensors, e-paper status
display, network time, serial console. One firmware for all boards and sensor sets; every
device-specific value is set on the console and stored in NVS.

Status: early, runs on the CrowPanel 5.79. See [docs/plan.md](docs/plan.md).

## Hardware (first target)

- Elecrow CrowPanel ESP32 E-Paper HMI 5.79" (DIS08792E): ESP32-S3, 8 MB flash, 8 MB PSRAM,
  792 x 272 b/w e-paper (2x SSD1683), USB-C via CH340C to UART0.
  Pin map and sources: [docs/hardware-crowpanel-579.md](docs/hardware-crowpanel-579.md)
- Sensors on I2C (SDA = GPIO3, SCL = GPIO8, header U5), all tested on the CrowPanel:
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
  license) and unpack it to `third_party/bsec3/` (git-ignored), so that
  `third_party/bsec3/release_bin/IAQ/bin/esp/esp32_s3/libalgobsec.a` exists. Then
  `idf.py reconfigure`. The build log says which BSEC it uses. Tested with BSEC 3.3.0.1.
- `HUB_FORCE_BSEC2=1 idf.py -B build-bsec2 build` builds the BSEC2 variant anyway.

Building means accepting Bosch's license terms for the BSEC version used.

## Build

Requires ESP-IDF v6.1.x.

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-10 flash monitor
```

On macOS the CH340C shows up as `/dev/cu.usbserial-*`. Exit the monitor with `Ctrl-]`.

## WLAN at build time (optional)

Create `wlan.config` in the repo root (git-ignored) and run `idf.py reconfigure`:

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
- `scd41 frc <ppm>` - forced recalibration; refused unless the sensor ran 3 min and CO2 was
  stable (span <= 40 ppm over 3 min). Outdoors use the fresh-air function instead:
- long press BOOT (or `scd41 fresh-air`): waits up to 30 min for stable CO2, then calibrates
  to `set co2 outdoor <ppm>` (default 430). Progress and result on the display.
- The display warns when CO2 was not below 450 ppm for more than 156 h.

`led on|off` switches the red LED (off after boot).

## Display and keys

Rotary switch up/down: pages (overview, network, time, one per sensor, system).
MENU or EXIT: back to the overview. Rotary press: full refresh (clears ghosting).
Long press BOOT: fresh-air calibration of the SCD41.
The overview shows the sensor values and up to 8 1-Wire probes; if WLAN or IPv4 is missing
for more than 20 s, a "NETWORK DOWN" overlay appears.

## Network and time defaults

- IPv4 via DHCP (hostname sent as option 12), IPv6 via SLAAC.
- NTP servers from DHCP option 42; without it 212.227.123.5 and 2001:8d8:fe:123::5.
  `set ntp` overrides both.
- Time zone `UTC0` (POSIX TZ string), changeable with `set tz`.

## License

MIT, see [LICENSE](LICENSE). Generated fonts: DejaVu (Bitstream Vera license),
see `components/ui/fonts/LICENSE-DejaVu`.

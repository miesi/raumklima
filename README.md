# raumklima-hub

ESP-IDF firmware for a room climate sensor hub: ESP32-S3, I2C sensors, e-paper status
display, network time, serial console. One firmware for all boards and sensor sets; every
device-specific value is set on the console and stored in NVS.

Status: early, runs on the CrowPanel 5.79. See [docs/plan.md](docs/plan.md).

## Hardware (first target)

- Elecrow CrowPanel ESP32 E-Paper HMI 5.79" (DIS08792E): ESP32-S3, 8 MB flash, 8 MB PSRAM,
  792 x 272 b/w e-paper (2x SSD1683), USB-C via CH340C to UART0.
  Pin map and sources: [docs/hardware-crowpanel-579.md](docs/hardware-crowpanel-579.md)
- Sensors on I2C (SDA = GPIO3, SCL = GPIO8, header U5):
  - BME680 - temperature, humidity, pressure, gas resistance
  - BH1750 - illuminance

Implemented, not yet tested on hardware: DS2484 I2C-to-1-Wire bridge with up to 8 DS18B20.
Planned: SCD41 (CO2), SHT40/SHT45 (reference temperature/humidity), BME690 (IAQ via BSEC).

BME680 IAQ uses Bosch BSEC2, downloaded at build time (closed binary, Bosch license - not
in this repo). Building with it means accepting Bosch's BSEC license terms.

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

## Display and keys

Rotary switch up/down: pages (overview, network, time, one per sensor, system).
MENU or EXIT: back to the overview. Rotary press: full refresh (clears ghosting).

## Network and time defaults

- IPv4 via DHCP (hostname sent as option 12), IPv6 via SLAAC.
- NTP servers from DHCP option 42; without it 212.227.123.5 and 2001:8d8:fe:123::5.
  `set ntp` overrides both.
- Time zone `UTC0` (POSIX TZ string), changeable with `set tz`.

## License

MIT, see [LICENSE](LICENSE). Generated fonts: DejaVu (Bitstream Vera license),
see `components/ui/fonts/LICENSE-DejaVu`.

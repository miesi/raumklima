# CLAUDE.md - raumklima hub

Guidance for Claude Code in `hub/`. The rules for the whole repository (git, licenses,
writing style, tests) are in `../CLAUDE.md`. All paths here are relative to `hub/`.

## Project

ESP-IDF (v6.1.x, C) firmware for an ESP32-S3 room climate sensor hub. Plan: `docs/plan.md`.
Board facts with sources: `docs/hardware-crowpanel-579.md`. System plan (nodes, roles, KNX):
`../docs/plan.md`.

## Environment

- ESP-IDF lives in `../../esp-idf` (v6.1.1), next to the repository. `export.sh` is NOT
  sourced in a fresh shell. Run every idf.py command as one line from `hub/`:
  `. ../../esp-idf/export.sh >/dev/null && idf.py build`
- Target `esp32s3`. Serial port of the CrowPanel: `/dev/cu.usbserial-10` (CH340C, UART0).
- Build: `tools/build.sh` (sources export.sh, prints only diagnostics, full log in
  `build/last-build.log`).
- `idf.py monitor` is interactive and never returns - do not run it from an agent. Use
  `~/.espressif/python_env/idf6.1_py3.15_env/bin/python ../tools/serial_run.py --reset -t 20`
  for a boot log, `-c "show status"` to run console commands.
- Never burn eFuses (`espefuse burn_*`). `espefuse summary` is fine.
- Flashing changes the device the user works with; say before you flash.
- Opening the serial port can reset the CH340 board. Put a test with more than one step into
  one `serial_run.py` session (`-c "@sleep 60"` waits inside it).
- Host tests (plain C parts, no ESP-IDF): `tools/test_host.sh`.
- KNX addresses from Thomas: individual addresses 1.2.30 to 1.2.39 for the hubs (this hub:
  1.2.30), group addresses 8/0/10 to 8/0/99 for tests. Use no other group address.
- KNX in the test network (FRITZ!Box, 192.168.178.0/24): there is no KNX router. Multicast
  tests with the test group addresses are permitted. Receiver on the Mac:
  `../tools/receivers/knx_listen.py --iface-ip <mac ip>` (the multicast route of the Mac goes
  to the VPN).
- KNX in a network with the router: its filters are off, so every telegram from the hub
  reaches the real KNX line. For tests, use `knx test-target <mac ip>` (unicast, RAM only), or
  multicast to the test group addresses only.
- Before a test with send addresses: `knx off` + `save`, then the test, then `knx on`
  without `save` - a reset in the test then leaves KNX off. Delete the test group addresses
  after the test.

## ESP-IDF v6 rules

- I2C: only the new driver (`driver/i2c_master.h`), one bus handle shared via `components/i2cbus`.
- SNTP: `esp_netif_sntp.h` API, not the raw lwIP `sntp_*` calls.
- Volatile APIs: check headers in `../../esp-idf/components` before using a function from memory.

## Architecture rules

- `main/app_main.c` only initialises components in order; no logic there.
- Components communicate via `esp_event` and the `metrics` component, not via each other's
  internals. The UI and the console never touch driver state; they read `metrics` and call the
  API in `sensors.h`. The only bus access outside the sensors task is the console's `i2c scan`,
  under `sensors_bus_lock()`.
- New sensor that only measures = new `components/sensors/drivers/<model>.c` implementing
  `sensor_driver_t` (`components/sensors/sensor_driver.h`), plus one line in the registry in
  `sensors.c`. No other file should need changes.
- A sensor with its own procedures (calibration, a bus of sub-devices - today SCD4x and DS2484)
  additionally exports `sensors_<name>_*` functions in `sensors.h` (`sensors_scd4x_*`,
  `sensors_onewire_*`), implemented in its driver file. Whatever talks to the sensor only posts
  a request; the bus work runs in the sensors task. Console commands and UI actions for it call
  only those functions.
- Drivers find other sensors' values by quantity (`metrics_get_primary()`), never by a fixed
  instance name such as "sht4x".
- Metric keys: `<instance>.<quantity>_<unit>`, units in the name (`_c`, `_pct`, `_hpa`, `_ppm`,
  `_lx`, `_ohm`). Gauges with `sensor_publish()`, states (accuracy, status, setting, counter)
  with `sensor_publish_state()` - states are never averaged.
- Timing: a driver that needs a measuring cadence declares `cadence_us` and `cadence_why`; all
  others leave it 0, return `SENSOR_DONE` at the end of a read cycle and are read once per
  sensor interval by the registry. Errors: `return sensor_fail(s, err)`. Interval boundaries
  only via `sched.h`.
- Log lines: `ESP_LOGx(tag, "event=<what> key=value ...")`. The tag is the subsystem (component
  or sensor instance name) and becomes the syslog MSGID; values with spaces in double quotes.
  No free text - netlog would send it as msg="...".
- Every device-specific value (WLAN, hostname, NTP, TZ, names) lives in NVS via `appcfg` and is
  settable on the console. Compile-time values are defaults only.
- Limits (`CONFIG_HUB_SENSORS_MAX`, `CONFIG_HUB_METRICS_MAX`, `OW_MAX_DEVICES`) are enforced
  with an error message and the first devices found keep working - never a silent truncation.
- Overview CO2 tile (Thomas' decision): with an SCD4x only its measurement, "--" while it is
  faulty - never the BSEC `co2eq_ppm` estimate as a stand-in. Without any SCD4x the estimate
  is shown, captioned "CO2 eq. (BSEC est.)".
- Overview tiles show only sensors that measure the room air (`metrics_get_ambient()`), not
  by-products such as the SCD4x temperature and humidity.
- Board pins only in `components/board/`. Nothing else hard-codes a GPIO number.
- Every long-running task that does not block on input registers with `health_register()` and
  calls `health_alive(id, interval_us)` once per loop pass; interval = its longest regular
  wait. A pass that takes longer than 2 x interval + 30 s aborts the hub (core dump).
- Big buffers (> some KB) static in PSRAM with `EXT_RAM_BSS_ATTR`, not `heap_caps_malloc`:
  the core dump copies every used heap block, PSRAM ones too, and would not fit.
- Timestamps sent off-device are UTC; TZ is for display only.
- Role: only the primary hub (`appcfg.role`) sends to Carbon and KNX and answers KNX reads.
- KNX byte layouts only from `components/knx/knx_proto.c`; it is plain C and checked against
  calimero-core 2.6 values in `components/knx/test/`.

## Third-party code

- In use: Bosch BME68x SensorAPI (BSD-3), Sensirion drivers (BSD-3), LVGL (MIT), vendored or
  via the IDF component manager, license file kept.
- `third_party/bsec3/` holds Thomas's BSEC3 download: never commit, never copy code from its
  examples (Bosch license); headers and the library are used from there at build time.
- Never run a real SCD41 FRC (`scd41 frc`, fresh-air) indoors - it mis-calibrates the sensor.
- Not allowed in the repo: Elecrow example code (no license), Bosch BSEC binaries (Bosch license),
  GPL code (GxEPD2, Linux drivers) - those were read for facts only.
- Fonts: `tools/mkfont.py` generates `components/ui/fonts/*.c` from DejaVu (needs Pillow).

## Writing style

See `../CLAUDE.md`. Config snippets (sdkconfig.defaults, partitions.csv) get a comment per
non-obvious line.

## Testing

Each milestone in `docs/plan.md` has a device test. A change is done when its test passed on
the hardware, not when it compiles. Report test output as is.

# CLAUDE.md - raumklima-hub

Guidance for Claude Code in this repository.

## Project

ESP-IDF (v6.1.x, C) firmware for an ESP32-S3 room climate sensor hub. Plan: `docs/plan.md`.
Board facts with sources: `docs/hardware-crowpanel-579.md`. The repo will be published on
GitHub - everything committed must be fit for that (no secrets, no unlicensed third-party code).

## Environment

- ESP-IDF lives in `../esp-idf` (v6.1.1). `export.sh` is NOT sourced in a fresh shell.
  Run every idf.py command as one line: `. ../esp-idf/export.sh >/dev/null && idf.py build`
- Target `esp32s3`. Serial port of the CrowPanel: `/dev/cu.usbserial-10` (CH340C, UART0).
- Build: `tools/build.sh` (sources export.sh, prints only diagnostics, full log in
  `build/last-build.log`).
- `idf.py monitor` is interactive and never returns - do not run it from an agent. Use
  `~/.espressif/python_env/idf6.1_py3.15_env/bin/python tools/serial_run.py --reset -t 20`
  for a boot log, `-c "show status"` to run console commands.
- Never burn eFuses (`espefuse burn_*`). `espefuse summary` is fine.
- Flashing changes the device the user works with; say before you flash.

## ESP-IDF v6 rules

- I2C: only the new driver (`driver/i2c_master.h`), one bus handle shared via `components/i2cbus`.
- SNTP: `esp_netif_sntp.h` API, not the raw lwIP `sntp_*` calls.
- Volatile APIs: check headers in `../esp-idf/components` before using a function from memory.

## Architecture rules

- `main/app_main.c` only initialises components in order; no logic there.
- Components communicate via `esp_event` and the `metrics` component, not via each other's
  internals. The UI never calls sensor drivers.
- New sensor = new `components/sensors/drivers/<model>.c` implementing `sensor_driver_t`
  (`components/sensors/sensor_driver.h`), plus one line in the registry in `sensors.c`.
  No other file should need changes.
- Metric keys: `<instance>.<quantity>_<unit>`, units in the name (`_c`, `_pct`, `_hpa`, `_ppm`,
  `_lx`, `_ohm`).
- Every device-specific value (WLAN, hostname, NTP, TZ, names) lives in NVS via `appcfg` and is
  settable on the console. Compile-time values are defaults only.
- Board pins only in `components/board/`. Nothing else hard-codes a GPIO number.
- Timestamps sent off-device are UTC; TZ is for display only.

## Third-party code

- Allowed: code with a license compatible with publishing (Bosch BME68x SensorAPI BSD-3,
  Sensirion drivers BSD-3), vendored or via the IDF component manager, license file kept.
- Not allowed in the repo: Elecrow example code (no license), Bosch BSEC binaries (Bosch license).

## Writing style

- Code, comments, commit messages and docs in English, ASCII only: `-` not dashes, `...` not
  ellipsis, straight quotes, no arrows, no emojis.
- Commit messages: imperative summary line, body says why. Write the message to a file and use
  `git commit -F <file>`.
- In docs, mark claims as VERIFIED (with source) or ASSUMPTION.
- Config snippets (sdkconfig.defaults, partitions.csv) with a comment per non-obvious line.

## Testing

Each milestone in `docs/plan.md` has a device test. A change is done when its test passed on
the hardware, not when it compiles. Report test output as is.

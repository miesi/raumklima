# raumklima

Room climate measurement for a home: hubs with a display, battery sensor nodes in the rooms
and outdoors, and data to Graphite (Carbon), syslog and KNX.

| Part | Hardware | Status |
|---|---|---|
| [hub](hub/README.md) | ESP32-S3 (Elecrow CrowPanel 5.79 e-paper), I2C sensors, WLAN | runs, see [hub/docs/plan.md](hub/docs/plan.md) |
| indoor node | nRF54L15, SHT4x, DS2484 with DS18B20, Chirp soil moisture, TMAG5273 water level, Sharp memory LCD, LiPo | planned |
| outdoor node | nRF54L15, SHT4x, DS2484 with DS18B20, Chirp soil moisture, 2 x AA Li-FeS2 | planned |

The system plan is in [docs/plan.md](docs/plan.md).

## How the parts work together

- Each hub measures the room air with its own sensors and shows the values on its display.
- The nodes send their values through Bluetooth LE. Each hub in range receives them (planned).
- One hub is the primary hub. Only the primary hub sends data to Carbon and to KNX. The other
  hubs send their values to the primary hub through the WLAN (planned).
- All hubs send log lines to syslog.

## Repository layout

| Path | Content |
|---|---|
| `hub/` | hub firmware (ESP-IDF v6.1) |
| `node/` | node firmware (nRF Connect SDK), planned |
| `protocol/` | radio protocol between node and hub, planned |
| `docs/` | system plan |
| `tools/receivers/` | local test receivers: go-carbon (Carbon) and rsyslogd (syslog) |
| `tools/serial_run.py` | serial console helper for scripts |

## License

MIT, see [LICENSE](LICENSE). The hub uses third-party parts with their own licenses, see
[hub/README.md](hub/README.md).

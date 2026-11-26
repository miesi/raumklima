# raumklima - system plan

Status 2026-11-26: the hub operates (see `hub/docs/plan.md`). The nodes, the radio protocol,
the operation with more than one hub and KNX are not built at this time.

## Why

One hub measures one room. Thomas wants values from more rooms, from the balcony and from
plant pots. Battery nodes measure these locations. The hubs show all values. One hub sends
all values to Carbon and to KNX. Thus the data has one source.

## Decisions

Thomas, 2026-11-25 and 2026-11-26:

- One repository for hub, node and protocol: github.com/miesi/raumklima.
- There is no Home Assistant or other smart-home system. Compatibility with such systems is
  not a goal.
- Node SoC: nRF54L15. Water level sensor: TI TMAG5273 (I2C Hall sensor).
- Quantity: 2 or 3 hubs, a maximum of 8 nodes.
- Intervals: configurable from 30 s to 3600 s. The measurement interval is from 240 s to
  3600 s. (Open: question 1.)
- Battery life: outdoor node 1 year with 2 x AA Li-FeS2. Indoor node: charge each 6 months
  (LiPo, USB-C).
- Time on the node: the node shows the time only after a time sync with the hub. If a node
  with a pairing gets no data from the primary hub for 3600 s, it shows a notice. It
  continues to show the time for 24 h. After that, it shows only the sensor values.
- Operation of a node without a pairing: optional.
- Seconds on the node display: configurable, for the measurement of the power consumption.
- Indoor display: Sharp Memory Display breakout 1.3 inch, 168 x 144 pixels, monochrome. The
  node has no buttons.
- Node firmware update: with a cable only.
- Registry: a console command writes the configuration to a dump and reads it back. Each hub
  can become the primary hub.
- The radio data is encrypted.
- Chirp soil moisture sensor: I2C version. Cables: 3 m maximum. A maximum of 3 DS18B20 for
  each node. The nodes read the DS18B20 with 10 bit resolution: 187.5 ms for each
  conversion, not 750 ms at 12 bit (DS18B20 datasheet).
- KNX: the primary hub sends the sensor values as KNXnet/IP routing telegrams to the KNX
  multicast address. Format: DPT 9.x. Interval: from 240 s to 3600 s. All hubs listen to
  configured group addresses. See "KNX".

## Radio: Bluetooth LE (proposal 2026-11-25)

Proposal: Bluetooth LE, no Thread and no Matter. Thomas' answer about the range (no mesh,
primary hub in a central location) agrees with it. Tell me if this is correct (question 2).

Facts:

- The ESP32-S3 has Wi-Fi and Bluetooth LE 5.0, but no IEEE 802.15.4 radio (VERIFIED:
  `esp-idf/components/soc/esp32s3/include/soc/soc_caps.h`). For Thread, it is necessary to add a
  second chip to the hub. The Espressif example for a Thread border router uses an ESP32-S3 with an
  ESP32-H2 on a UART (VERIFIED: `esp-idf/examples/openthread/ot_br/README.md`).
- NimBLE in ESP-IDF has the LE Coded PHY (long range) (VERIFIED: Kconfig
  `BT_NIMBLE_LL_CFG_FEAT_LE_CODED_PHY`, default y). A range test on the hardware is
  necessary.
- BTHome v2 uses BLE advertising and AES-CCM encryption with a 16 byte key (VERIFIED:
  bthome.io/format). It has object IDs for moisture, battery, voltage, distance in mm and a
  UTC timestamp (VERIFIED: same source).
- Matter: the soil sensor device type starts with Matter 1.5 (VERIFIED: Silicon Labs blog
  "Matter 1.5"). A developer report says that Apple Home does not show this device type
  (ASSUMPTION: forum report, not an Apple statement).

How it operates:

- Uplink: the node sends advertising packets in the BTHome v2 layout, encrypted. Without Home
  Assistant, BTHome is only a documented layout with a known encryption. We do not make a new
  format.
- Downlink (time, name, key, configuration): a small GATT service of our own. The hub connects to
  the node for a short time, for example one time each day for the time.
- Range: the audio connection from Thomas' phone to the headphones stops after 3/4 of
  the hallway. Audio is Bluetooth Classic with a high data rate. Advertising packets are small, and
  the Coded PHY gives a better link budget (ASSUMPTION: test on the hardware). Each hub
  receives the nodes in its range. The primary hub does not have to receive all nodes.
- Risk: Wi-Fi and Bluetooth use one radio on the ESP32-S3. Continuous BLE scan operation can
  decrease the Wi-Fi data rate (ASSUMPTION: measure it).

## Node

- Supply of the outdoor node: 2 x AA Li-FeS2 in series, 3.0 V to 3.6 V. The nRF54L15 operates
  from 1.7 V to 3.6 V (ASSUMPTION: examine the datasheet).
- Power budget: approximately 3000 mAh in 8760 h gives a mean current of 340 uA for 1 year
  (ASSUMPTION: capacity at low current and low temperature). BLE advertising each 30 s uses
  much less. The Chirp sensor and the DS18B20 conversions use most of the energy. Measure the
  current with a power analyzer.
- Battery level: the voltage of Li-FeS2 cells stays almost flat and changes with the
  temperature. Thus the voltage alone does not give a good percentage. Proposal: measure the
  voltage with a load and count the used energy in the firmware. LiPo: a fuel gauge IC
  (ASSUMPTION: for example MAX17048), because the charge through USB-C makes a count
  incorrect.
- I2C cables of 3 m: risk (ASSUMPTION). A low clock and strong pull-up resistors can be
  sufficient. If not, use an I2C bus buffer. Test it with the cables of the installation.
- Time: the hub sends UTC and the POSIX time zone string. The node calculates the summer
  time. A 32.768 kHz crystal with 20 ppm gives approximately 2 s each day (ASSUMPTION: crystal
  data). One time sync each day is sufficient for a display in minutes.
- Pairing: a new node sends packets with a flag "no pairing". The hub shows "new node". The
  operator gives the name on the hub. The hub writes the name and a new key to the node.

## More than one hub

- Role: each hub is "primary" or "secondary". Only the primary hub sends to Carbon and KNX.
  Proposal: an explicit setting `set role primary|secondary`.
- The hubs find each other in the WLAN with mDNS (DNS-SD). Each hub tells its role and if
  Carbon is on. If two hubs are primary or send to Carbon, all hubs show a warning.
- Data between the hubs: UDP multicast in the WLAN, not Bluetooth. Each hub sends its values
  and the node values that it received, one time in each interval. Plain text in step one,
  the same as syslog and Carbon.
- Registry (node ID, name, key, alarm limits): the primary hub keeps it and sends copies to
  the other hubs. All hubs must have the keys to decrypt the node data. `config dump` and
  `config restore` move the registry to a different hub.

## KNX

Facts:

- KNXnet/IP routing uses the multicast address 224.0.23.12 (VERIFIED:
  `knxToDbWriter/src/main/java/de/mieslinger/myknxreader/GroupMonitorMulticast.java`).
  UDP port 3671 (ASSUMPTION: KNX standard, examine it with the calimero source).
- DPT 9.x is a 2 byte floating point value. The subtype gives the unit: 9.001 temperature
  in degrees C, 9.004 illuminance in lux, 9.006 pressure in Pa, 9.007 humidity in %, 9.008
  air quality in ppm (ASSUMPTION: KNX standard, examine it with the calimero source).

Proposal:

- Send (primary hub only): at each KNX interval boundary (240 s to 3600 s, aligned to UTC),
  the hub sends the mean of the interval for each configured group address. The hub sends
  the telegrams with a small gap between them, so that a KNX IP router does not get too many
  telegrams at the same time (ASSUMPTION: gap of 50 ms).
- Listen (all hubs): the hub keeps the last value and the time of the last telegram for each
  configured group address.
- Console commands (proposal for the syntax):
  - `knx address <area.line.device>`: set the individual address of the hub.
  - `knx ga add <main/middle/sub> <dpt> send <metric> | listen [name]`
  - `knx ga list`: all group addresses with DPT, direction, last value and the time of the
    last telegram.
  - `knx ga del <main/middle/sub>`
  - `set interval knx <s>`
- Test receiver: knxToDbWriter (Java, MariaDB), or a small Python listener in
  `tools/receivers/` for automatic tests (question 6).

## Steps

Each step has a result that Thomas can see.

1. Repository structure (complete 2026-11-26).
2. Hub: HTTP endpoint for the core dump, safe mode screen.
3. Hub: KNX send and listen, console commands, test with a receiver.
4. Hub: roles, mDNS, warnings, data between the hubs.
5. Protocol v0 and a node on the nRF54L15 DK with an SHT4x only. The hub receives the values
   and shows them. Range test.
6. Pairing, names, registry, `config dump` and `config restore`.
7. Time sync and Sharp memory display.
8. Indoor node complete: DS2484, Chirp, TMAG5273, LiPo, fuel gauge.
9. Outdoor node, measurement of the power consumption, calculation of the battery life.

## Open questions

The open questions are in the chat of 2026-11-26. Their answers go into "Decisions".

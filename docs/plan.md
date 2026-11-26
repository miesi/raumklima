# raumklima - system plan

Status 2026-11-26: the hub operates (see `hub/docs/plan.md`), with KNX send and listen (tested
in the test network with unicast and multicast, see "KNX"). The nodes, the radio protocol and the operation with
more than one hub are not built at this time.

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
- Measurement interval: indoor node 30 s to 3600 s, outdoor node 240 s to 3600 s. The radio
  interval is the same as the measurement interval. After each packet, a node with a display
  keeps its receiver on for some milliseconds. In this time the primary hub sends the time.
- Display interval of the node: each second for a test of the power consumption, else each
  minute.
- Battery life: outdoor node 1 year with 2 x AA Li-FeS2. Indoor node: charge each 6 months
  (LiPo, USB-C).
- Time on the node: the node shows the time only after a time sync with the hub. Only the
  primary hub sends the time, as a reply to the sensor data of the node. If more than one hub
  replies, a node with a display shows a warning message. If a node
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
- Radio: Bluetooth LE, no Thread, no Matter (Thomas, 2026-11-26).
- KNX: the primary hub sends the sensor values as KNXnet/IP routing telegrams to the KNX
  multicast address. Format: DPT 9.x. Interval: from 240 s to 3600 s. All hubs listen to
  configured group addresses. See "KNX".
  - Each value gets its own group address, set with a command (for example
    `knx ga add 3/1/10 9.001 send bme690.temperature_c`).
  - The primary hub sends its own values and the values of the other hubs and the nodes. It
    does not send received KNX values again.
  - The value is always the mean of the interval.
  - Received values: only on the KNX page of the display. Not to Carbon, not as an input.
  - The primary hub answers GroupValueRead for its send addresses.
  - The filter table of the KNX IP router is off (small installation: all telegrams on the
    two sides). The hub must not fill the 9600 bit/s TP line.
- Roles (proposal, Thomas agreed): an explicit command `set role primary|secondary`, default
  primary. Thus one hub operates without an adjustment. A secondary hub does not send to Carbon
  or KNX. It does not answer KNX reads and does not send the time to the nodes.
- knxToDbWriter (`../knxToDbWriter`, a different repository): a different agent does its test
  suite and its update. This repository uses it only as a receiver for tests.
- KNX addresses: individual addresses 1.2.30 to 1.2.39 for the hubs, one address for each
  hub. Group addresses 8/0/10 to 8/0/99 for tests. The test network (FRITZ!Box,
  192.168.178.0/24) has no KNX router and no knxToDbWriter.
- Alarms, for example low battery and dew point: the node with a display shows its alarms.
  All hubs show the alarms of all nodes.

## Radio: Bluetooth LE

Decision (Thomas, 2026-11-26): Bluetooth LE, no Thread and no Matter.

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
- Time: after each packet, a node with a display keeps its receiver on for some milliseconds.
  The primary hub replies in this window with the time (ASSUMPTION: the ESP32-S3 replies in
  sufficient time; measure it in step 5). BLE 5.4 PAwR is made for this, but the ESP32-S3 has
  Bluetooth 5.0 only.
- Other downlink data (name, key, configuration): a small GATT service of our own. The hub
  connects to the node for a short time.
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
- Byte layouts (DPT 9, routing indication, search request, search response): VERIFIED with
  calimero-core 2.6. `hub/components/knx/test/test_knx_proto.c` has the calimero values as
  a host test (`hub/tools/test_host.sh`).
- DPT 9 units (VERIFIED: calimero 2.6): 9.001 degrees C, 9.004 lx, 9.006 Pa, 9.007 %, 9.008
  ppm. Maximum value 670433.28. The resolution decreases with the value: at 1000 hPa (100000
  Pa) a step is 81.92 Pa, approximately 0.8 hPa.
- TP1 line: approximately 23 ms for one DPT 9 telegram with ACK, a maximum of approximately
  43 telegrams each second (ASSUMPTION: calculated from the TP1 timing, not measured).

How it operates (hub, 2026-11-26):

- Send: at each boundary of the KNX interval (default 300 s) the primary hub sends one
  GroupValueWrite for each send address. The gap between two telegrams is 200 ms or more.
  Thus the hub sends a maximum of 5 telegrams each second, approximately 12 % of the TP1 line
  during the burst. ROUTING_BUSY from a router stops the sending for the time that it asks for.
- Listen: all hubs keep the last value, the source and the time of the last telegram.
- Commands: `knx`, `knx on|off`, `knx address`, `knx search`, `knx monitor <s>`,
  `knx ga add|list|del`, `set interval knx`, `set role`, `knx test-target <ip>|off`.
- `knx test-target <ip>`: the hub sends all telegrams unicast to a test receiver, not to the
  multicast group. Nothing goes to the KNX line. The setting is in RAM only.

Test 2026-11-26 with `tools/receivers/knx_listen.py` and the test target (VERIFIED):

- GroupValueRead to a send address: the hub replied with the value (before the first boundary
  with the last reading). A telegram with the address of the hub as source: ignored.
- At the boundary 18:56:00 UTC: 5 writes, 121 ms after the boundary, with gaps of 201, 202,
  210 and 195 ms at the receiver. (Before a correction: up to 332 ms, because the task looked
  at the queue only each 100 ms.)
- A write to a listen address: `knx ga list` shows 3.50 degrees C, the source and the time.
- A send address with a DPT that does not agree with the unit of the metric is refused.

Test 2026-11-26 with multicast 224.0.23.12 in the test network, hub address 1.2.30, test
group addresses 8/0/10, 8/0/11 and 8/0/20 (VERIFIED: hub console and
`knx_listen.py --iface-ip 192.168.178.22` on the Mac):

- Hub to Mac: the reply to a GroupValueRead (22.76 degrees C) and the boundary writes
  (22.80 degrees C and 51.28 %, 205 ms gap) arrived at the Mac.
- Mac to hub: a GroupValueRead and 10 of 10 writes (20.01 to 20.10, 1 s gap) arrived at the
  hub. One write before this test did not arrive. 3 s after that write the hub recorded a
  Wi-Fi beacon timeout. Later the FRITZ!Box changed from channel 11 to channel 6.
- Wi-Fi does not send a multicast frame again if it does not arrive. KNXnet/IP routing also
  does not send a telegram again (ASSUMPTION: from the 802.11 and KNXnet/IP specifications, not
  measured over a long time). Thus one value can be missing. The next interval sends a new value.
- The reply to the read arrived 1.8 s after the read (ASSUMPTION: the access point keeps
  multicast frames for a station in power save until the next DTIM beacon).
- On the Mac the route for 224.0.0.0/4 goes to the VPN (`utun4`). Thus `knx_listen.py` sets
  `IP_MULTICAST_IF` from `--iface-ip`.
- No KNX router answered `knx search`. This is correct: the test network has no KNX router.

Open:

- A test with the KNX router and knxToDbWriter in the network where they are installed.
  Use only the test group addresses 8/0/10 to 8/0/99.
- The KNX page on the display: built, not examined on the panel.

## Steps

Each step has a result that Thomas can see.

1. Repository structure (complete 2026-11-26).
2. Hub: HTTP endpoint for the core dump, safe mode screen (complete 2026-11-26).
3. Hub: KNX send and listen, console commands, test with a receiver (complete 2026-11-26,
   unicast and multicast in the test network. The test with the KNX router is open).
4. Hub: roles, mDNS, warnings, data between the hubs. Each hub tells its KNX individual
   address. If two hubs have the same address, all hubs show a warning: a hub ignores the
   telegrams with its own address as source.
5. Protocol v0 and a node on the nRF54L15 DK with an SHT4x only. The hub receives the values
   and shows them. Range test.
6. Pairing, names, registry, `config dump` and `config restore`.
7. Time sync and Sharp memory display.
8. Indoor node complete: DS2484, Chirp, TMAG5273, LiPo, fuel gauge.
9. Outdoor node, measurement of the power consumption, calculation of the battery life.

## Open questions

- Dew point alarm: which comparison. Proposal: the dew point of the air (SHT4x temperature
  and humidity) against a surface temperature (a DS18B20 at the window or the wall). Alarm
  when the surface temperature is less than the dew point plus a margin.
- Alarm limits: where are they set. Proposal: in the registry on the primary hub, one set for
  each node, with copies to the other hubs and to the node.
- Alarm list: low battery and dew point. Are other alarms necessary (for example low water
  level, dry soil, no data from a node)? Do the hubs also send alarms to syslog?
- KNX: when and where is the test with the KNX router.
- Hub time: after the first time sync after a boot, the clock was 4 s behind (see
  `hub/docs/plan.md`). Correct this before step 7, because the nodes receive the time from the
  primary hub.

The open questions are also in the chat of 2026-11-26. Their answers go into "Decisions".

# Hardware: Elecrow CrowPanel ESP32 E-Paper HMI 5.79" (DIS08792E)

Every line is marked VERIFIED (with source) or ASSUMPTION.

Sources:

- [SCH] Elecrow schematic `Eagle_SCH&PCB/CrowPanel-ESP32-Display-5.79E-Inch/CrowPanel ESP32 Display-5.79(E) Inch.sch`
  in https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792 (parsed 2026-11-16)
- [SPI] `example/arduino/Examples/5.79_GPIO/spi.h` and `EPD_Init.h` in the same repo
- [ESPTOOL] `esptool flash-id` and `espefuse summary` against the board on 2026-11-16
- [WIKI] https://elecrow.com/wiki/CrowPanel_ESP32_E-paper_5.79-inch_HMI_Display.html

## SoC

| Item | Value | Status |
|---|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual core 240 MHz | VERIFIED [ESPTOOL] |
| Flash | 8 MB, quad, 3.3 V (eFuse) | VERIFIED [ESPTOOL] |
| PSRAM | 8 MB embedded, vendor AP_3v3 | VERIFIED [ESPTOOL] |
| PSRAM mode | octal (OPI) | ASSUMPTION - 8 MB in-package is octal on ESP32-S3R8; Elecrow also says OPI. Confirm with first boot log |
| eFuse STRAP_JTAG_SEL | 0, so GPIO3 is NOT evaluated as strapping pin | VERIFIED [ESPTOOL] |
| USB | Type-C via CH340C (U1) to UART0 (TXD0/RXD0), auto-reset via 2x S9013 | VERIFIED [SCH], USB VID:PID 1a86:7523 on the Mac |
| Native USB (GPIO19/20) | not connected to the Type-C port; GPIO19/20 go to the header | VERIFIED [SCH] |
| Battery | 3.7 V Li-Ion connector, charger 4054A (U6), regulator RY3420 (U2); no GPIO for battery voltage found | VERIFIED [SCH] (no ADC net to the battery) |

## GPIO map

| GPIO | Net | Function | Status |
|---|---|---|---|
| 0 | BOOT | BOOT button / auto-download | VERIFIED [SCH] |
| 1 | IO1_EXIT | EXIT button, active low | VERIFIED [SCH], [WIKI] |
| 2 | IO2_MENU | MENU button, active low | VERIFIED [SCH], [WIKI] |
| 3 | IO3 | header U5 pin 1, nothing else | VERIFIED [SCH] |
| 4 | IO4_DOWN | rotary switch down | VERIFIED [SCH] |
| 5 | IO5_CONF | rotary switch press | VERIFIED [SCH] |
| 6 | IO6_UP | rotary switch up | VERIFIED [SCH] |
| 7 | IO7_LCD_3.3_CTL | e-paper power rail enable, HIGH = on | VERIFIED [SCH] (net), level from Elecrow examples |
| 8 | IO8 | header U5 pin 20, nothing else | VERIFIED [SCH] |
| 9 | IO9 | header U5 pin 2 | VERIFIED [SCH] |
| 10 | IO10_SPI_CS | microSD CS | VERIFIED [SCH] |
| 11 | IO11_SPI_MOSI | e-paper SDA (MOSI) | VERIFIED [SCH], [SPI] |
| 12 | IO12_SPI_CLK | e-paper SCL (SCK) | VERIFIED [SCH], [SPI] |
| 13 | IO13_SPI_MISO | microSD MISO | VERIFIED [SCH] |
| 14 | IO14 | header U5 pin 19 | VERIFIED [SCH] |
| 15 | IO15 | header U5 pin 3 | VERIFIED [SCH] |
| 16 | IO16 | header U5 pin 18 | VERIFIED [SCH] |
| 17 | IO17 | header U5 pin 4 | VERIFIED [SCH] |
| 18 | IO18 | header U5 pin 17 | VERIFIED [SCH] |
| 19 | IO19 | header U5 pin 5 (native USB D- on ESP32-S3, unused here) | VERIFIED [SCH] |
| 20 | IO20 | header U5 pin 16 (native USB D+ on ESP32-S3, unused here) | VERIFIED [SCH] |
| 21 | IO21 | header U5 pin 6 | VERIFIED [SCH] |
| 38 | IO38 | header U5 pin 15 | VERIFIED [SCH] |
| 39 | IO39_SPI_CLK | microSD CLK | VERIFIED [SCH] |
| 40 | IO40_SPI_MOSI | microSD MOSI | VERIFIED [SCH] |
| 41 | IO41_LED | power LED | VERIFIED [SCH] |
| 42 | IO42_TF_3.3_CTL | microSD power rail enable | VERIFIED [SCH] (net); level ASSUMPTION HIGH = on |
| 43/44 | TXD0/RXD0 | UART0 to CH340C | VERIFIED [SCH] |
| 45 | IO45_CS | e-paper CS (strapping pin VDD_SPI, ignored: flash voltage set by eFuse) | VERIFIED [SCH], [SPI], [ESPTOOL] |
| 46 | IO46_D/C | e-paper D/C (strapping pin, boot message printing) | VERIFIED [SCH], [SPI] |
| 47 | IO47_RES | e-paper reset | VERIFIED [SCH], [SPI] |
| 48 | IO48_BUSY | e-paper BUSY | VERIFIED [SCH], [SPI] |

Header U5 (2x10): pins 1-6 = IO3, IO9, IO15, IO17, IO19, IO21; 7-10 = GND; 11-14 = 3V3;
15-20 = IO38, IO20, IO18, IO16, IO14, IO8. VERIFIED [SCH]. The physical position of pin 1
on the board is NOT verified - check the silkscreen before wiring.

No I2C pull-ups on the board. IO3 and IO8 only go to the header and a test pad. VERIFIED [SCH].

## E-paper panel

| Item | Value | Status |
|---|---|---|
| Resolution | 792 x 272, black/white | VERIFIED [WIKI] |
| Controller | 2x SSD1683 cascaded (master/slave), 396 columns each | VERIFIED [WIKI], comment in [SPI] `EPD_Init.h` |
| Framebuffer | 800 x 272 because of an 8-column gap at the seam; visible area 792 x 272 | VERIFIED comment in [SPI] `EPD_Init.h` |
| Refresh modes | full, fast, partial (register 0x22 sequences) | VERIFIED [SPI] `EPD_Init.cpp` |
| Elecrow driver license | none declared in the GitHub repo | VERIFIED (GitHub API, `license: null`) |

Because the Elecrow code has no license, this project does not copy it. The driver is written
against the SSD1683 datasheet; the Elecrow code is only used to confirm pins and the cascade
geometry.

## I2C pin choice

Requested: SDA = GPIO3, SCL = GPIO8. Assessment: keep it.

- GPIO3 is a strapping pin (JTAG source select), but it is only evaluated when eFuse
  `STRAP_JTAG_SEL` = 1. It is 0 on this board (VERIFIED [ESPTOOL]). Even if it were set, the
  pull-up holds GPIO3 high at reset, which selects USB-JTAG - the default anyway.
- GPIO8 has no special function on ESP32-S3 and is unused on the board.
- Both are routed only to the header (VERIFIED [SCH]); nothing else on the board drives them.
- No onboard pull-ups: the sensor breakouts must provide them (most do, typically 4.7-10 kOhm).

Pins to avoid for I2C on this board: GPIO19/20 (native USB on the planned XIAO/QT Py builds,
keeping the pin map portable), GPIO0/45/46 (strapping), GPIO43/44 (console UART).

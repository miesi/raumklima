#!/usr/bin/env python3
"""Non-interactive serial helper (for scripts and agents; idf.py monitor is interactive).

Reads the console for a fixed time, optionally resets the board first and sends
console commands. Run with the ESP-IDF python env (needs pyserial).

  serial_run.py [-p PORT] [--reset] [-t SECONDS] [-c "show status" -c ...]
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/cu.usbserial-10")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--time", type=float, default=10.0, help="seconds to read after the last command")
    ap.add_argument("--reset", action="store_true", help="pulse EN via RTS before reading")
    ap.add_argument("-c", "--cmd", action="append", default=[], help="console command, repeatable")
    ap.add_argument("--cmd-delay", type=float, default=1.5, help="seconds before each command")
    a = ap.parse_args()

    # DTR drives IO0 through the auto-reset transistor: keep it inactive, or the chip boots
    # into the ROM download mode.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, a.baud, 0.1
    s.dtr = False
    s.rts = False
    s.open()
    if a.reset:
        s.rts = True    # EN low
        time.sleep(0.1)
        s.rts = False
    out = sys.stdout

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            data = s.read(4096)
            if data:
                out.write(data.decode("utf-8", "replace"))
                out.flush()

    for c in a.cmd:
        pump(a.cmd_delay)
        s.write((c + "\r\n").encode())
    pump(a.time)
    s.close()


if __name__ == "__main__":
    main()

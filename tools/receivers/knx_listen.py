#!/usr/bin/env python3
"""KNXnet/IP routing listener and test sender (no dependencies).

Listens to the KNX multicast group 224.0.23.12, UDP 3671, and prints each telegram:
source, destination group address, service (read, response, write), data and the data as
DPT 9 (2 byte float).

  knx_listen.py [-t SECONDS] [--src 15.15.250] [--ga 31/7/250] [--iface-ip 192.168.178.22]

Discovery (IP side only, the router answers itself, nothing goes to the TP line):

  knx_listen.py --search                   SEARCH_REQUEST, print the KNXnet/IP devices

Sending (only with an explicit flag; with the router filters off, the telegram also goes
to the KNX TP line - use a group address that no device uses):

  knx_listen.py --read 31/7/250            send a GroupValueRead, print the responses
  knx_listen.py --write 31/7/250 21.5      send a GroupValueWrite with DPT 9

Test without the KNX line: "knx test-target <this host>" on the hub sends unicast here;
"--to <hub>" sends --read/--write unicast to the hub.
"""
import argparse
import socket
import struct
import sys
import time

MCAST = "224.0.23.12"
PORT = 3671
APCI = {0x000: "read", 0x040: "response", 0x080: "write"}


def ga_str(v):
    return "%d/%d/%d" % (v >> 11, (v >> 8) & 7, v & 0xFF)


def ia_str(v):
    return "%d.%d.%d" % (v >> 12, (v >> 8) & 0xF, v & 0xFF)


def ga_parse(s):
    m, mi, su = (int(x) for x in s.split("/"))
    return (m << 11) | (mi << 8) | su


def ia_parse(s):
    a, l, d = (int(x) for x in s.split("."))
    return (a << 12) | (l << 8) | d


def dpt9_decode(b):
    raw = (b[0] << 8) | b[1]
    if raw == 0x7FFF:
        return None  # invalid data
    m = raw & 0x7FF
    if raw & 0x8000:
        m -= 2048
    e = (raw >> 11) & 0xF
    return 0.01 * m * (1 << e)


def dpt9_encode(v):
    """Same result as calimero 2.6 (DPTXlator2ByteFloat): divide first, round at the end."""
    x = v * 100.0
    e = 0
    while x < -2048.0 or x > 2047.0:
        x /= 2.0
        e += 1
    m = int(x + 0.5) if x >= 0 else -int(-x + 0.5)
    if m > 2047:
        m //= 2
        e += 1
    if e > 15:
        raise ValueError("out of DPT 9 range")
    return struct.pack(">H", ((m & 0x800) << 4) | (e << 11) | (m & 0x7FF))


def frame(src, dst, apci, data=b""):
    """KNXnet/IP ROUTING_INDICATION with a cEMI L_Data.ind to a group address."""
    tpdu = bytes([0x00 | (apci >> 8), apci & 0xFF]) + data
    # ctrl1 0x9c, ctrl2 0xe0: the same as calimero 2.6 for a group telegram, priority low
    cemi = bytes([0x29, 0x00, 0x9C, 0xE0]) + struct.pack(">HH", src, dst) + bytes([len(tpdu) - 1]) + tpdu
    return struct.pack(">BBHH", 0x06, 0x10, 0x0530, 6 + len(cemi)) + cemi


def parse(pkt):
    if len(pkt) < 6 or pkt[0] != 0x06 or pkt[1] != 0x10:
        return None
    svc = struct.unpack(">H", pkt[2:4])[0]
    if svc != 0x0530:
        return {"service": "0x%04x" % svc}
    c = pkt[6:]
    if len(c) < 2 or c[0] != 0x29:
        return {"service": "routing", "cemi": "0x%02x" % c[0]}
    c = c[2 + c[1]:]  # skip the additional info
    if len(c) < 8:
        return None
    ctrl2 = c[1]
    src, dst = struct.unpack(">HH", c[2:6])
    ln = c[6]
    tpdu = c[7:8 + ln]
    apci = ((tpdu[0] & 0x03) << 8) | (tpdu[1] & 0xC0)
    small = tpdu[1] & 0x3F if ln == 1 else None
    data = tpdu[2:] if ln > 1 else b""
    return {"src": ia_str(src), "dst": ga_str(dst) if ctrl2 & 0x80 else ia_str(dst),
            "group": bool(ctrl2 & 0x80), "apci": APCI.get(apci, "0x%03x" % apci), "small": small,
            "data": data}


def search(local_ip, wait):
    """SEARCH_REQUEST to the multicast group; each KNXnet/IP server answers with a
    SEARCH_RESPONSE (unicast) that holds its name, individual address and routing group."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    u.bind((local_ip, 0))
    u.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local_ip))
    port = u.getsockname()[1]
    hpai = struct.pack(">BB4sH", 8, 0x01, socket.inet_aton(local_ip), port)
    u.sendto(struct.pack(">BBHH", 0x06, 0x10, 0x0201, 6 + len(hpai)) + hpai, (MCAST, PORT))
    u.settimeout(0.5)
    end = time.time() + wait
    while time.time() < end:
        try:
            pkt, peer = u.recvfrom(1024)
        except socket.timeout:
            continue
        if len(pkt) < 6 or struct.unpack(">H", pkt[2:4])[0] != 0x0202:
            continue
        dib = pkt[6 + 8:]  # after the control endpoint HPAI
        if len(dib) >= 54 and dib[1] == 0x01:
            ia = struct.unpack(">H", dib[4:6])[0]
            mc = socket.inet_ntoa(dib[14:18])   # DIB DEVICE_INFO: multicast at 14, MAC at 18 (calimero)
            name = dib[24:54].split(b"\0")[0].decode("ascii", "replace")
            print("found %s: name=%r address=%s routing_group=%s medium=0x%02x" % (peer[0], name, ia_str(ia), mc,
                  dib[2]), flush=True)
        else:
            print("found %s: %s" % (peer[0], pkt.hex()), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--time", type=float, default=60.0)
    ap.add_argument("--src", help="print only telegrams from this individual address")
    ap.add_argument("--ga", help="print only telegrams to this group address")
    ap.add_argument("--iface-ip", default="0.0.0.0", help="local IPv4 address for the multicast join")
    ap.add_argument("--me", default="15.15.249", help="source address for --read/--write")
    ap.add_argument("--search", action="store_true", help="find KNXnet/IP devices (IP side only)")
    ap.add_argument("--to", help="send --read/--write unicast to this IPv4 address (the hub), not to the group")
    ap.add_argument("--read", metavar="GA")
    ap.add_argument("--write", nargs=2, metavar=("GA", "VALUE"))
    a = ap.parse_args()
    if a.search:
        if a.iface_ip == "0.0.0.0":
            sys.exit("--search needs --iface-ip")
        search(a.iface_ip, 5)
        return

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", PORT))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 socket.inet_aton(MCAST) + socket.inet_aton(a.iface_ip))
    if a.iface_ip != "0.0.0.0":
        # send to the group on this interface, not on the route for 224/4 (on the Mac: the VPN)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(a.iface_ip))
    s.settimeout(0.5)

    me = ia_parse(a.me)
    dest = (a.to or MCAST, PORT)
    if a.read:
        s.sendto(frame(me, ga_parse(a.read), 0x000), dest)
        print("sent read to %s" % a.read, flush=True)
    if a.write:
        s.sendto(frame(me, ga_parse(a.write[0]), 0x080, dpt9_encode(float(a.write[1]))), dest)
        print("sent write %s = %s" % (a.write[0], a.write[1]), flush=True)

    end = time.time() + a.time
    n = 0
    while time.time() < end:
        try:
            pkt, peer = s.recvfrom(1024)
        except socket.timeout:
            continue
        t = parse(pkt)
        if not t or "src" not in t:
            if t:
                print("%s %s %s" % (time.strftime("%H:%M:%S"), peer[0], t), flush=True)
            continue
        if (a.src and t["src"] != a.src) or (a.ga and t["dst"] != a.ga):
            continue
        n += 1
        d = t["data"]
        v = ""
        if len(d) == 2:
            dv = dpt9_decode(d)
            v = " dpt9=%s" % ("invalid" if dv is None else "%.2f" % dv)
        small = "" if t["small"] is None else " small=%d" % t["small"]
        now = time.time()
        stamp = time.strftime("%H:%M:%S", time.localtime(now)) + ".%03d" % int((now % 1) * 1000)
        print("%s %-15s %-10s -> %-9s %-8s data=%s%s%s" % (stamp, peer[0], t["src"], t["dst"],
              t["apci"], d.hex(), small, v), flush=True)
    print("telegrams: %d in %.0f s" % (n, a.time), file=sys.stderr)


if __name__ == "__main__":
    main()

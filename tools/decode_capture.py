#!/usr/bin/env python3
"""Decode a SeeZero sniffer log (lines like "[  22.073 s]  49 B: AA AA B0 ...").

Usage: python3 decode_capture.py capture.txt [--raw] [--since SECONDS]
"""
import argparse, re

ATTR_NAMES = {
    (0x0702, 0x0000): ("Meter reading (delivered)", "div"), (0x0702, 0x0001): ("Meter reading (received/export)", "div"),
    (0x0702, 0x0400): ("Instantaneous demand", "W"), (0x0702, 0x0401): ("Today", "div"),
    (0x0702, 0x0403): ("Yesterday", "div"), (0x0702, 0x0430): ("This week", "div"),
    (0x0702, 0x0440): ("This month", "div"), (0x0702, 0x0C01): ("Gas today (kWh)", "div"),
    (0x0702, 0x0C03): ("Gas yesterday (kWh)", "div"), (0x0702, 0x0C30): ("Gas this week (kWh)", "div"),
    (0x0702, 0x0C40): ("Gas this month (kWh)", "div"), (0x0702, 0x0301): ("Multiplier", None),
    (0x0702, 0x0302): ("Divisor", None), (0x0702, 0x0014): ("Supply status", None),
    (0x0702, 0x0200): ("Meter status", None), (0x0702, 0x0306): ("Metering device type", None),
    (0x0705, 0x051C): ("Cost today", "gbp"), (0x0705, 0x051E): ("Cost yesterday", "gbp"),
    (0x0705, 0x0530): ("Cost this week", "gbp"), (0x0705, 0x0540): ("Cost this month", "gbp"),
    (0x0700, 0x0301): ("Standing charge", "gbp"), (0x0700, 0x0302): ("Conversion factor (raw)", None),
    (0x0700, 0x0304): ("Calorific value (raw)", None),
}
SKIP_ATTRS = {0x0307}  # site ID (MPAN/MPRN): never printed
# cluster 0x0708 (device management) is skipped entirely for the same reason


def crc16_arc(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def load(path):
    out = []
    for line in open(path, errors="ignore"):
        m = re.match(r"\[\s*([\d.]+) s\]\s+\d+ B:(.*)", line)
        if m:
            try:
                out += [(float(m.group(1)), int(h, 16)) for h in m.group(2).split()]
            except ValueError:
                pass
    return out


def frames(stream):
    i, bad = 0, 0
    while i < len(stream) - 6:
        if stream[i][1] == 0xAA and stream[i + 1][1] == 0xAA:
            ln = stream[i + 3][1] | stream[i + 4][1] << 8
            n = 8 + ln
            if ln <= 1024 and i + n <= len(stream):
                f = bytes(b for _, b in stream[i:i + n])
                if crc16_arc(f[2:-2]) == (f[-2] << 8 | f[-1]):
                    yield stream[i][0], f
                    i += n
                    continue
                bad += 1
        i += 1
    frames.bad = bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--raw", action="store_true", help="also print undecoded frame types")
    ap.add_argument("--since", type=float, default=0)
    a = ap.parse_args()
    div = {0: 1000, 1: 1000}
    for t, f in frames(load(a.capture)):
        if t < a.since:
            continue
        typ, p = f[2], f[6:-2]
        if typ == 0xB0 and len(p) >= 4:
            cl, dev, j, out = p[1] | p[2] << 8, p[3], 4, []
            if cl == 0x0708:  # device management (supplier/device IDs): never printed
                continue
            while j + 4 <= len(p):
                at, n = p[j] | p[j + 1] << 8, p[j + 2] | p[j + 3] << 8
                v = p[j + 4:j + 4 + n]
                j += 4 + n
                if at in SKIP_ATTRS:
                    continue
                val = int.from_bytes(v, "little", signed=(cl == 0x0702 and at == 0x0400))
                if cl == 0x0702 and at == 0x0302 and val:
                    div[dev] = val
                name, kind = ATTR_NAMES.get((cl, at), (f"0x{at:04X}", None))
                if kind == "div":
                    s = f"{val / div.get(dev, 1000):g}"
                elif kind == "gbp":
                    s = f"£{val / 100000:.5f}"
                elif kind == "W":
                    s = f"{val * 1000 / div.get(dev, 1000):g} W"
                else:
                    s = str(val)
                out.append(f"{name}={s}")
            print(f"{t:8.1f}s {['ELEC', 'GAS'][dev] if dev < 2 else dev} 0x{cl:04X}: " + ", ".join(out))
        elif typ == 0xD0 and len(p) >= 6:
            cl, cmd, dev, n = p[0] | p[1] << 8, p[2], p[3], p[4] | p[5] << 8
            s = p[6:6 + n]
            fuel = ["ELEC", "GAS"][dev] if dev < 2 else str(dev)
            if cl == 0x0700 and cmd == 0 and n >= 88:
                td = s[87] if s[87] <= 9 else s[87] >> 4
                print(f"{t:8.1f}s {fuel} Publish Price: unit rate £{int.from_bytes(s[76:80], 'little') / 10**td:.5f}/kWh")
            elif cl == 0x0700 and cmd == 4 and n >= 38:
                td = s[37] if s[37] <= 9 else s[37] >> 4
                print(f"{t:8.1f}s {fuel} Publish Tariff Information: standing charge £{int.from_bytes(s[16:20], 'little') / 10**td:.5f}/day")
            elif cl == 0x0703 and cmd == 0 and n >= 12:
                print(f"{t:8.1f}s Display Message: {s[12:12 + s[11]].decode('latin1')!r}")
            elif a.raw:
                print(f"{t:8.1f}s D0 cluster 0x{cl:04X} cmd {cmd:02X} dev {dev}: {s.hex(' ')}")
        elif a.raw:
            print(f"{t:8.1f}s type {typ:02X}: {p.hex(' ')}")
    print(f"(frames failing CRC: {getattr(frames, 'bad', 0)})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bring-up lab for FT93xx-family sensors (FTE3600/FTE6600 ACPI, chip 0x9391 etc.).

Speaks the register protocol directly over spidev, so individual steps of the
libfprint driver's sequence can be replayed, reordered and inspected. Run as root.

  ft93xx-lab.py probe                      reset, idle, read chip id
  ft93xx-lab.py regs                       dump the interesting register window
  ft93xx-lab.py image [-n N] [--pgm F]     image-mode configure + N captures
  ft93xx-lab.py fdt-image [--fix ...]      image capture, FDT configure+arm, image capture again
  ft93xx-lab.py touch [--fix ...]          arm finger detect, wait IRQ, capture
"""
import argparse, array, fcntl, os, select, struct, sys, time

import spidev

SPI_DEV = os.environ.get("FT_SPI", "/dev/spidev0.0")
GPIO_DEV = os.environ.get("FT_GPIO", "/dev/fte3600-gpio-FTE6600:00")
UIO_DEV = os.environ.get("FT_UIO", "/dev/fte3600-irq-FTE6600:00")

PROFILES = {  # id: (width, height, window, extension, dac, gain, polarity)
    0x9391: (40, 196, 0xC7FE, 0x7FFF, 124, 5, 3),
    0x9392: (40, 196, 0xC7FE, 0x7FFF, 124, 5, 3),
    0x9395: (36, 176, 0xAFFE, 0x0FFF, 120, 8, 0),
    0x9397: (32, 176, 0xAFFE, 0x0FFF, 97, 11, 3),
}

# ---------------------------------------------------------------- GPIO reset (gpio-cdev v2)
GPIO_V2_LINE_FLAG_ACTIVE_LOW = 1 << 1
GPIO_V2_LINE_FLAG_OUTPUT = 1 << 3
GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES = 2
GPIO_V2_GET_LINE_IOCTL = (3 << 30) | (592 << 16) | (0xB4 << 8) | 0x07
GPIO_V2_LINE_SET_VALUES_IOCTL = (3 << 30) | (16 << 16) | (0xB4 << 8) | 0x0F


class Reset:
    """Claims the glue's single 'reset' line exactly like linux_board.c: OUTPUT|ACTIVE_LOW,
    logical 0 = released (physical high), logical 1 = asserted (physical low)."""

    def __init__(self):
        self.chip = os.open(GPIO_DEV, os.O_RDONLY)
        # struct gpio_v2_line_request
        offsets = struct.pack("64I", *([0] * 64))
        consumer = b"ft93xx-lab".ljust(32, b"\0")
        attr = struct.pack("IIQQ", GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES, 0, 0, 1)  # values=0, mask=1
        attrs = attr + b"\0" * (24 * 9)
        config = struct.pack("QI5I", GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW, 1, 0, 0, 0, 0, 0) + attrs
        tail = struct.pack("II5Ii", 1, 0, 0, 0, 0, 0, 0, 0)
        req = bytearray(offsets + consumer + config + tail)
        assert len(req) == 592, len(req)
        fcntl.ioctl(self.chip, GPIO_V2_GET_LINE_IOCTL, req)
        self.fd = struct.unpack("i", req[-4:])[0]

    def set(self, asserted):
        fcntl.ioctl(self.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, struct.pack("QQ", 1 if asserted else 0, 1))

    def pulse(self):
        self.set(1); time.sleep(0.010)
        self.set(0); time.sleep(0.020)

    def close(self):
        os.close(self.fd); os.close(self.chip)


# ---------------------------------------------------------------- wire protocol
class Wire:
    def __init__(self, cs_high=False, speed=4_000_000, trace=False):
        self.spi = spidev.SpiDev()
        bus, cs = SPI_DEV.replace("/dev/spidev", "").split(".")
        self.spi.open(int(bus), int(cs))
        self.spi.mode = 0 | (0x04 if cs_high else 0)
        self.spi.max_speed_hz = speed
        self.spi.bits_per_word = 8
        self.trace = trace
        self.n = 0

    def xfer(self, tx):
        rx = self.spi.xfer2(list(tx))
        self.n += 1
        if self.trace:
            print(f"    spi> {bytes(tx[:8]).hex()}{'..' if len(tx) > 8 else ''} < {bytes(rx[:12]).hex()}{'..' if len(rx) > 12 else ''}")
        return rx

    def command(self, c):
        self.xfer([c, (~c) & 0xFF, 0])

    def read_byte(self, a):
        return self.xfer([0x08, 0xF7, a, 0, 0])[4]

    def write_byte(self, a, v):
        self.xfer([0x09, 0xF6, a, v & 0xFF])

    def read_word(self, a, check_crc=True):
        rx = self.xfer([0x04, 0xFB, 0x80 | (a >> 8), a & 0xFF, 0, 0, 0, 0, 0, 0])
        if check_crc and (rx[8] or rx[9]):
            crc = 0xFFFF
            for b in rx[6:8]:
                crc ^= b << 8
                for _ in range(8):
                    crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
            if crc != (rx[8] << 8 | rx[9]):
                raise IOError(f"CRC mismatch reading 0x{a:04x}: {bytes(rx).hex()}")
        return rx[6] << 8 | rx[7]

    def write_word(self, a, v):
        self.xfer([0x05, 0xFA, 0x80 | (a >> 8), a & 0xFF, 0, 0, (v >> 8) & 0xFF, v & 0xFF])

    def masked_write(self, a, mask, bits):
        self.write_word(a, (self.read_word(a) & mask) | bits)

    def fifo(self, length):
        out = bytearray()
        while len(out) < length:
            chunk = min(1790, length - len(out))
            wc = chunk // 2 - 1
            rx = self.xfer([0x06, 0xF9, 0x9A, 0x05, wc >> 8, wc & 0xFF] + [0] * (chunk + 2))
            out += bytes(rx[6:6 + chunk])
        return out


def ms(t): time.sleep(t / 1000.0)


# ---------------------------------------------------------------- driver sequence replicas
class Sensor:
    def __init__(self, w: Wire, verbose=True):
        self.w = w
        self.v = verbose
        self.chip = self.raw = 0
        self.mode = 0xA0
        self.configured = False
        self.fdt_configured = False
        self.threshold_cached = 0xAA  # forces first 0x180c write like the driver
        self.fdt_dac = 122
        self.state = dict(dac=124, gain=5, polarity=3, integration=128, sample_time=15, sample_mode=2, subtract_baseline=1)

    def log(self, *a):
        if self.v: print(*a)

    # --- handshake ------------------------------------------------------------
    def idle(self):
        self.w.command(0x5A); ms(1)
        v = self.w.read_byte(0x80)
        i = 0
        while v != 0x50 and i < 5:
            self.w.command(0xC0); ms(1)
            v = self.w.read_byte(0x80)
            if v != 0x50: ms(1)
            i += 1
        self.w.command(0xA5)
        ack = v == 0x50
        if not ack: self.log(f"  idle: NOT acknowledged (0x80=0x{v:02x} after {i} retries)")
        return ack

    def spi_mode(self):
        for _ in range(4):
            self.w.write_byte(0xC6, 1); ms(4)
            if self.w.read_byte(0xC6) == 1: return True
        self.log("  spi_mode: 0xc6 never read back 1"); return False

    def probe(self):
        for _ in range(5):
            self.w.write_byte(0xFD, 0x0A); self.w.write_byte(0xFE, 0x7F); ms(1)
            if self.w.read_byte(0xFE) == 0x7F: break
        else:
            self.log("  probe: voltage register 0xfe never read back 0x7f")
        self.spi_mode()
        for _ in range(3):
            word = self.w.read_word(0x1A8B)
            self.w.write_word(0x1A84, 0xFFFF)
            if word in PROFILES:
                self.chip = self.raw = word
                if word == 0x9391 and self.w.read_word(0x1816) == 0x0FFF:
                    self.chip = 0x9395
                return word
            self.idle()
        return word

    def startup(self):
        self.idle()
        cid = self.probe()
        self.log(f"  chip id 0x{cid:04x} (profile 0x{self.chip:04x})")
        if self.chip not in PROFILES:
            raise SystemExit(f"unsupported chip 0x{cid:04x}")
        self.spi_mode()
        self.mode = 0xA0; self.configured = False
        self.w.write_word(0x1A84, 0xFFFF)
        p = PROFILES[self.chip]
        self.state.update(dac=p[4], gain=p[5], polarity=p[6])
        return cid

    # --- image mode (ft93xx.c configure) ---------------------------------------
    def configure_image(self, mode=0xA3, force_full=False):
        p = PROFILES[self.chip]; s = self.state; w = self.w
        dac = 0x3000 | (s["polarity"] << 14) | (s["gain"] << 8) | s["dac"]
        timing = ((s["integration"] - 1) << 5) | s["sample_time"]
        prior = self.mode
        self.idle()
        w.write_word(0x1801, dac); w.write_word(0x1800, p[2]); w.write_word(0x1816, p[3])
        w.write_word(0x1807, timing)
        rate = getattr(self, "rate", "fast")
        RATES = {"fast": (0x023B, 0x0001, 0x0104, 0x0100), "2m": (0x04BB, 0x0482, 0x0207, 0x0209), "slow": (0x09BB, 0x0987, 0x0491, 0x0493)}
        if rate != "skip":
            t = RATES[rate]
            w.write_word(0x1806, t[0]); w.write_word(0x180A, t[1]); w.write_word(0x180B, t[2])
            v = w.read_word(0x1812)
            w.write_word(0x1812, (v & 0x8000) | (s["subtract_baseline"] << 14) | t[3])
        self.mode = mode
        if prior in (0xA3, 0xA4) and not force_full:
            return "short(prior image mode)"
        if s["sample_mode"] >= 0:
            w.write_word(0x1804, 0x27C0 | s["sample_mode"] | (8 if s["sample_mode"] > 1 else 0))
        if self.configured and not force_full:
            return "partial(configured)"
        w.write_word(0x1811, 0x01FE)
        for bit in (0x20, 0x40):
            v = w.read_word(0x1A83)
            if not v & bit: w.write_word(0x1A83, v | bit)
        self.configured = True
        for a, v in ((0x8E, 0xF4), (0x8D, 0x61), (0x8F, 0xFF), (0x90, 0), (0x91, 7), (0x92, 0xD0), (0x90, 1)):
            w.write_byte(a, v)
        w.write_word(0x1A06, 1)
        return "full"

    def nav_prepare(self):
        """navigation_capture's one-time prep (calibration path): 0x1808 nav bits + row registers."""
        w = self.w
        self.idle()
        v = w.read_word(0x1808); w.write_word(0x1808, (v & 0xC7FF) | 0x0800)
        w.write_byte(0x9A, 0x5A)
        rows = 25 if self.chip in (0x9391, 0x9392) else 22
        for row in range(rows):
            a = 0xC1 + 2 * row
            bits = 7 if row == 0 else 3 if self.chip > 0x9390 else 4
            v = w.read_word(a); w.write_word(a, (v & 0xFFC0) | bits)
        w.write_byte(0x9A, 0)
        self.nav_rows = rows

    def capture(self, polls=100, poll_ms=1, read_anyway=True, scan_cmd=0xC4, nav=False, start=True, peek=False):
        """configure + scan cmd + scan start + ready poll + FIFO. Returns dict.
        nav=True replicates navigation_capture (0xc8, mode 0xa4, nav row count)."""
        p = PROFILES[self.chip]; w = self.w
        if nav and not getattr(self, "nav_rows", 0):
            self.nav_prepare()
        how = self.configure_image(0xA4 if nav else 0xA3)
        if nav: scan_cmd = 0xC8
        expect = {0xC4: 0x54, 0xC8: 0x58}[scan_cmd]
        w.command(scan_cmd); ms(1)
        for i in range(10):
            b = w.read_byte(0x80)
            if b == expect: break
        mode_ok = b == expect
        st = 0; n = -1; dt = 0
        if start:
            irqfd = None
            if getattr(self, "irq_wait", 0):
                irqfd = os.open(UIO_DEV, os.O_RDONLY | os.O_NONBLOCK)
                try: os.read(irqfd, 4)
                except BlockingIOError: pass
            w.write_word(0x1800, p[2] | 1)
            if irqfd is not None:
                t1 = time.monotonic(); pl = select.poll(); pl.register(irqfd, select.POLLIN)
                ev = pl.poll(int(self.irq_wait * 1000))
                print(f"    irq: {'fired' if ev else 'none'} after {(time.monotonic() - t1) * 1000:.1f} ms")
                os.close(irqfd)
            if getattr(self, "settle_ms", 0):
                ms(self.settle_ms)
            if peek:
                for k in range(4):
                    print(f"    peek {k}: 0x80={w.read_byte(0x80):02x} 1a82={w.read_word(0x1A82, check_crc=False):04x} 1800={w.read_word(0x1800, check_crc=False):04x}")
            t0 = time.monotonic()
            for n in range(polls):
                st = w.read_word(0x1A82)
                if st & 0x20:
                    w.write_word(0x1A84, st); break
                ms(poll_ms)
            dt = (time.monotonic() - t0) * 1000
        ready = bool(st & 0x20)
        r = dict(configure=how, cmd=f"0x{scan_cmd:02x}", mode_ack=mode_ok, started=start, ready=ready, polls=n, status=st, wait_ms=round(dt, 1))
        if start and (ready or read_anyway):
            width = p[0]
            rows = self.nav_rows if nav else p[1] + (4 if self.chip in (0x9391, 0x9392) else 0)
            height = self.nav_rows if nav else p[1]
            raw = w.fifo(width * rows * 2)
            w.write_word(0x1A84, 0x20)
            px = [((raw[2 * i] << 8 | raw[2 * i + 1]) & 0x0FFC) for i in range(width * height)]
            r.update(px=px, width=width, height=height, raw=raw)
            r.update(stats(px))
        r["alive_after"] = w.read_word(0x1A8B, check_crc=False) == 0x9391
        return r

    # --- finger detect mode (ft93xx.c configure_fdt / fdt_arm) ------------------
    def configure_fdt(self, replacement=700, threshold_mode=0, force_full=False):
        skip = getattr(self, "fdt_skip", set())
        class _W:  # wrapper that drops writes to skipped addresses
            def __init__(self, w): self.w = w
            def __getattr__(self, n): return getattr(self.w, n)
            def write_word(self, a, v):
                if a in skip: return
                self.w.write_word(a, v)
            def masked_write(self, a, mask, bits):
                if a in skip: return
                self.w.masked_write(a, mask, bits)
        w = _W(self.w); s = self.state; c = self.chip
        gain = 7 if c in (0x9391, 0x9392) else 11 if c == 0x9397 else 10
        dac = 0x3000 | (s["polarity"] << 14) | (gain << 8) | self.fdt_dac
        self.idle()
        w.write_word(0x1801, dac)
        if self.threshold_cached != threshold_mode:
            w.write_word(0x180C, 0 if threshold_mode else (0x0400 | replacement)); self.threshold_cached = threshold_mode
        newer = c >= 0x9391
        w.masked_write(0x1807, 0xFFF0, 1)
        w.write_word(0x1806, 0x09BB if newer else 0x04BB); w.write_word(0x180A, 0x0987 if newer else 0x0482); w.write_word(0x180B, 0x0491 if newer else 0x0207)
        w.masked_write(0x1812, 0x8000, (s["subtract_baseline"] << 14) | (0x0493 if newer else 0x0209))
        if self.mode == 0xA1 and not force_full:
            return "short(prior fdt)"
        wide = c in (0x9391, 0x9392, 0x9395)
        w.write_word(0x1881, 0x20CE if wide else 0x0FCE)
        self.mode = 0xA1
        window, ext = {0x9391: (0x1C10, 0x0420), 0x9392: (0x1C10, 0x0420), 0x9395: (0x1A08, 0x0208), 0x9397: (0x1A08, 0x0208)}.get(c, (2, 0))
        w.write_word(0x1800, window)
        if newer: w.write_word(0x1816, ext)
        w.write_word(0x1804, 0x07C0)
        if self.fdt_configured and not force_full:
            return "partial(fdt configured)"
        mode = 0x084B if wide else 0x0807
        w.write_word(0x1808, mode); w.write_word(0x1815, 0); w.write_word(0x180D, 0x03E8); w.write_word(0x1880, 0x5F64)
        w.write_word(0x1884, 3); w.write_word(0x1887, 0 if wide else 1); w.write_word(0x1888, 0)
        w.write_byte(0x9A, 0x5A)
        row = 0 if c in (0x9391, 0x9392) else 1
        ch = ((3, 9, 15, 20), (2, 8, 13, 19))[row]
        bits = 0x0103 if row == 0 else 0x0083 if c == 0x9395 else 0x0080
        for n in ch:
            a = 0xC0 + 2 * n
            if "chan" in skip: continue
            w.masked_write(a, 0xE000, bits); w.masked_write(a + 1, 0xE000, 0)
        w.write_byte(0x9A, 0)
        v = w.read_word(0x1A83)
        if not v & 8 and "1a83" not in skip: w.write_word(0x1A83, v | 8)
        self.fdt_configured = True
        return "full"

    def fdt_measure(self):
        w = self.w
        self.configure_fdt()
        for _ in range(5):
            v = w.read_word(0x1881)
            if not v & 2: w.write_word(0x1881, v | 2)
            w.command(0xC2); ms(1)
            w.write_word(0x1885, 1)
            for _ in range(5):
                st = w.read_word(0x1A82)
                if st & 8:
                    w.write_word(0x1A84, 8); return True
                ms(1)
        self.log("  fdt_measure: ready bit 8 never set"); return False

    def fdt_read(self):
        rx = self.w.xfer([0x04, 0xFB, 0x80, 0xB8, 0, 3] + [0] * 10)
        return [rx[6 + 2 * i] << 8 | rx[7 + 2 * i] for i in range(4)]

    def fdt_write_base(self, values8):
        w = self.w
        w.write_byte(0x9A, 0x5A)
        w.xfer([0x05, 0xFA, 0x80, 0xB0, 0, 7] + sum(([v >> 8, v & 0xFF] for v in values8), []))
        w.write_byte(0x9A, 0)

    def fdt_arm(self, base8, down=True, features=0):
        w = self.w
        self.configure_fdt()
        if features & 2:
            w.write_word(0x1817, w.read_word(0x1817) & 0xFFFC); w.write_word(0x1889, 4)
        w.write_word(0x1A84, 0x002F)
        self.fdt_write_base(base8)
        v = w.read_word(0x1881)
        w.write_word(0x1881, (v & 0xFF7C) | (0x81 if down else 0))
        w.command(0xC2); ms(1)


# ---------------------------------------------------------------- helpers
def stats(px):
    n = len(px); srt = sorted(px)
    low = sum(1 for v in px if v < 8); sat = sum(1 for v in px if v >= 4092)
    return dict(n=n, min=srt[0], p5=srt[n // 20], median=srt[n // 2], p95=srt[-n // 20 - 1], max=srt[-1],
                mean=round(sum(px) / n, 1), below8_pct=round(100 * low / n, 1), sat_pct=round(100 * sat / n, 1))


def write_pgm(path, r):
    px = r["px"]; w, h = r["width"], r["height"]
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n255\n".encode()); f.write(bytes(v >> 4 for v in px))
    print(f"  wrote {path} ({w}x{h})")


CHAN = [0xC6, 0xC7, 0xD2, 0xD3, 0xDE, 0xDF, 0xE8, 0xE9]
REGS = CHAN + [0x1800, 0x1801, 0x1804, 0x1806, 0x1807, 0x1808, 0x180A, 0x180B, 0x180C, 0x180D, 0x1810, 0x1811, 0x1812, 0x1815, 0x1816, 0x1817,
        0x1880, 0x1881, 0x1884, 0x1885, 0x1887, 0x1888, 0x1889, 0x1A06, 0x1A82, 0x1A83, 0x1A84, 0x1A8B, 0x1A8E]


def dump_regs(w, label):
    vals = {}
    for a in REGS:
        try: vals[a] = w.read_word(a)
        except IOError as e: vals[a] = None
    print(f"  regs[{label}]: " + " ".join(f"{a:04x}={'----' if v is None else f'{v:04x}'}" for a, v in vals.items()))
    return vals


def diff_regs(a, b, la, lb):
    d = [(k, a[k], b[k]) for k in a if a[k] != b.get(k)]
    if d: print(f"  regs changed {la} -> {lb}: " + " ".join(f"{k:04x}:{x:04x}->{y:04x}" for k, x, y in d))
    else: print(f"  regs unchanged {la} -> {lb}")


def show(r, label):
    core = {k: v for k, v in r.items() if k not in ("px", "raw", "width", "height")}
    core["status"] = f"0x{core['status']:04x}"
    print(f"  capture[{label}]: {core}")


def wait_irq(timeout_s):
    fd = os.open(UIO_DEV, os.O_RDONLY | os.O_NONBLOCK)
    try:
        try: before = struct.unpack("I", os.read(fd, 4))[0]
        except BlockingIOError: before = None
        print(f"  waiting for IRQ on {UIO_DEV} (count before={before}) up to {timeout_s}s ...")
        p = select.poll(); p.register(fd, select.POLLIN)
        t0 = time.monotonic()
        ev = p.poll(int(timeout_s * 1000))
        if not ev: print("  no IRQ"); return False
        cnt = struct.unpack("I", os.read(fd, 4))[0]
        print(f"  IRQ after {time.monotonic() - t0:.3f}s (count={cnt})"); return True
    finally:
        os.close(fd)


def apply_fixes(s: Sensor, fixes, saved):
    """Remedies to try between finger-detect arm and image capture."""
    w = s.w
    for f in fixes:
        if f == "none": continue
        elif f == "clear":    w.write_word(0x1A84, 0xFFFF)
        elif f == "idle":     s.idle()
        elif f == "full":     s.configured = False; s.mode = 0xA0   # force full image configure
        elif f == "r1808":    w.write_word(0x1808, saved[0x1808])
        elif f == "r1881":    w.write_word(0x1881, saved[0x1881])
        elif f == "r1884":    w.write_word(0x1884, saved[0x1884])
        elif f == "r1880":    w.write_word(0x1880, saved[0x1880])
        elif f == "r1887":    w.write_word(0x1887, saved[0x1887]); w.write_word(0x1888, saved[0x1888])
        elif f == "r180c":    w.write_word(0x180C, saved[0x180C])
        elif f == "r180d":    w.write_word(0x180D, saved[0x180D]); w.write_word(0x1815, saved[0x1815])
        elif f == "r1815":    w.write_word(0x1815, 1)
        elif f == "r1815w":   w.write_word(0x1815, 1); ms(100)
        elif f == "r1815x":   w.write_word(0x1815, 0); ms(10); w.write_word(0x1815, 1); ms(100)
        elif f == "rchan":
            w.write_byte(0x9A, 0x5A)
            for a in CHAN: w.write_word(a, saved[a])
            w.write_byte(0x9A, 0)
        elif f == "all":
            apply_fixes(s, ["r1808", "r1881", "r1884", "r1880", "r1887", "r180c", "r180d", "rchan", "irqoff"], saved)
        elif f == "fdtoff":   w.write_word(0x1881, w.read_word(0x1881) & 0xFF7C)   # disarm (bits 0,1,7)
        elif f == "irqoff":   w.write_word(0x1A83, w.read_word(0x1A83) & ~8)
        elif f == "reset":    raise SystemExit("use --reset-between instead")
        else: raise SystemExit(f"unknown fix {f}")
        print(f"  applied fix: {f}")


# ---------------------------------------------------------------- commands
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["probe", "regs", "image", "fdt", "fdt-image", "touch", "hunt", "hunt2", "fdt-steps", "fulldiff", "fdt-bisect", "sweep", "dacsweep"])
    ap.add_argument("-n", type=int, default=1, help="captures")
    ap.add_argument("--pgm", help="write last image here (prefix for multiple)")
    ap.add_argument("--no-reset", action="store_true", help="skip the reset pulse")
    ap.add_argument("--cs-high", action="store_true", help="SPI_CS_HIGH polarity")
    ap.add_argument("--speed", type=int, default=4_000_000)
    ap.add_argument("--polls", type=int, default=100)
    ap.add_argument("--fix", default="none", help="comma list applied between FDT arm and capture: " +
                    "none,clear,idle,full,r1808,r1881,r1884,r1880,r1887,r180c,r180d,fdtoff,irqoff")
    ap.add_argument("--timeout", type=float, default=15.0, help="touch: IRQ wait seconds")
    ap.add_argument("--trace", action="store_true", help="print every SPI transfer")
    ap.add_argument("--scan-cmd", default="c4", choices=["c4", "c8"])
    ap.add_argument("--nav", action="store_true", help="replicate calibration navigation capture (0xc8, 25 rows)")
    ap.add_argument("--no-start", action="store_true", help="skip the 0x1800|1 scan start")
    ap.add_argument("--peek", action="store_true", help="read 0x80/0x1a82/0x1800 right after scan start")
    ap.add_argument("--fdt-first", action="store_true", help="run the full FDT configure before the first image capture")
    ap.add_argument("--rate", default="fast", choices=["fast", "2m", "slow", "skip"], help="AFE scan-rate register set for 0x1806/0x180a/0x180b/0x1812")
    ap.add_argument("--integration", type=int, default=128)
    ap.add_argument("--sample-time", type=int, default=15)
    ap.add_argument("--settle-ms", type=int, default=0, help="sleep this long after scan start before touching SPI")
    ap.add_argument("--irq-wait", type=float, default=0, help="wait up to N s for the INT line after scan start before touching SPI")
    ap.add_argument("--sample-mode", type=int, default=2, help="-1 skips the 0x1804 write")
    ap.add_argument("--no-baseline", action="store_true", help="subtract_baseline=0")
    ap.add_argument("--skip-first", action="store_true", help="fdt-image/touch: skip the image capture before FDT")
    ap.add_argument("--gain", type=int, default=-1, help="override image gain (0-15)")
    ap.add_argument("--dac", type=int, default=-1, help="override image dac (0-255)")
    ap.add_argument("--configs", default="", help="sweep: comma list of name=gain,dac,rate,integ,mode1804,fdt(0/1)")
    ap.add_argument("--unlock", action="store_true", help="dacsweep: write 0x1801 under the 0x9a unlock")
    ap.add_argument("--no-fdt", action="store_true", help="hunt: stay in the power-on (pre-FDT) analog state")
    ap.add_argument("--fdt-skip", default="", help="comma list of hex addrs / chan / 1a83 to omit in FDT configure")
    ap.add_argument("--pre-sleep", type=float, default=0, help="sleep after startup before first capture")
    ap.add_argument("--pre", default="", help="comma list of addr=val word writes applied before the first capture")
    a = ap.parse_args()

    if os.geteuid() != 0: raise SystemExit("run as root")
    rst = Reset()
    if not a.no_reset:
        rst.pulse(); print("reset pulsed")
    w = Wire(cs_high=a.cs_high, speed=a.speed, trace=a.trace)
    s = Sensor(w)
    s.startup()
    s.fdt_skip = {x if x in ("chan", "1a83") else int(x, 16) for x in filter(None, a.fdt_skip.split(","))}
    s.rate = a.rate; s.settle_ms = a.settle_ms; s.irq_wait = a.irq_wait
    s.state["integration"] = a.integration
    s.state["sample_time"] = a.sample_time
    s.state["sample_mode"] = a.sample_mode
    if a.no_baseline: s.state["subtract_baseline"] = 0
    if a.gain >= 0: s.state["gain"] = a.gain
    if a.dac >= 0: s.state["dac"] = a.dac
    print(f"startup done, 0x1816=0x{w.read_word(0x1816):04x}, {w.n} transfers")
    if a.cmd == "probe":
        dump_regs(w, "after-startup"); return
    if a.cmd == "regs":
        dump_regs(w, "after-startup"); return

    r0 = dump_regs(w, "after-startup")
    if a.pre_sleep: time.sleep(a.pre_sleep); print(f"  slept {a.pre_sleep}s")
    if a.fdt_first:
        print("  fdt configure:", s.configure_fdt()); dump_regs(w, "after-fdt-configure")
    for kv in filter(None, a.pre.split(",")):
        addr, val = (int(x, 16) for x in kv.split("="))
        w.write_word(addr, val); print(f"  pre-write 0x{addr:04x}=0x{val:04x}")
    if a.cmd == "fdt":
        print("  fdt configure:", s.configure_fdt()); dump_regs(w, "after-fdt-configure")
        for i in range(a.n):
            ok = s.fdt_measure(); vals = s.fdt_read()
            print(f"  fdt sample#{i}: measure_ok={ok} values={vals} alive={w.read_word(0x1A8B, check_crc=False) == 0x9391}")
        dump_regs(w, "after-fdt"); return
    if a.cmd == "hunt":
        if not a.no_fdt: print("  fdt configure:", s.configure_fdt())  # match the driver's post-arm register state
        best = None; t_end = time.monotonic() + a.timeout; frames = 0; last_report = 0
        while time.monotonic() < t_end:
            r = s.capture(polls=a.polls)
            frames += 1
            if not r["ready"] or not r["alive_after"]:
                print(f"  frame {frames}: ready={r['ready']} alive={r['alive_after']} -- stopping"); break
            spread = r["p95"] - r["p5"]
            if best is None or spread > best[0]:
                best = (spread, r, frames)
            now = time.monotonic()
            if now - last_report > 2:
                print(f"  t={a.timeout - (t_end - now):5.1f}s frames={frames} median={r['median']} p5={r['p5']} p95={r['p95']} spread={spread}")
                last_report = now
        if best:
            spread, r, idx = best
            print(f"  best frame #{idx}: spread={spread} min={r['min']} median={r['median']} max={r['max']}")
            if a.pgm: write_pgm(a.pgm, r)
        return
    if a.cmd == "dacsweep":
        # Does 0x1801 (polarity/gain/dac) move the image at all? Optionally under the 0x9a unlock.
        for pol in (0, 1, 2, 3):
            for gain in (5, 11):
                line = []
                for dac in (0, 64, 128, 192, 255):
                    s.state.update(dac_polarity=pol, gain=gain, dac=dac) if False else s.state.update(polarity=pol, gain=gain, dac=dac)
                    if a.unlock:
                        s.configure_image(0xA3)
                        v = 0x3000 | (pol << 14) | (gain << 8) | dac
                        w.write_byte(0x9A, 0x5A); w.write_word(0x1801, v); w.write_byte(0x9A, 0)
                    r = s.capture(polls=a.polls)
                    if not r["ready"] or not r["alive_after"]:
                        line.append(f"dac{dac}:STALL"); rst.pulse(); s.startup(); 
                        if a.fdt_first: s.configure_fdt()
                        continue
                    line.append(f"dac{dac}:{r['median']}")
                print(f"  pol={pol} gain={gain:2d} 0x1801 readback=0x{w.read_word(0x1801):04x}  " + " ".join(line))
        return
    if a.cmd == "sweep":
        # Each config: fresh reset, optional FDT configure, then reference frames, touch window, lift.
        default_cfgs = ["A_p3g5d0=5,0,fast,32,0x27ca,1,3", "B_p1g5d64=5,64,fast,32,0x27ca,1,1", "C_p3g11d64=11,64,fast,32,0x27ca,1,3",
                        "D_p1g11d64=11,64,fast,32,0x27ca,1,1", "E_p3g11d128=11,128,fast,32,0x27ca,1,3", "F_p1g11d96i128=11,96,fast,128,0x27ca,1,1",
                        "G_p3g11d64slow=11,64,slow,64,0x27ca,1,3", "H_p1g5d64m07c0=5,64,fast,32,0x07c0,1,1"]
        old_cfgs = ["g5d124fast=5,124,fast,16,0x27ca,0", "g7d124fast=7,124,fast,16,0x27ca,0", "g9d124fast=9,124,fast,16,0x27ca,0",
                        "g11d124fast=11,124,fast,8,0x27ca,0", "g5d64fast=5,64,fast,16,0x27ca,0", "g5d200fast=5,200,fast,16,0x27ca,0",
                        "g5d124slow=5,124,slow,32,0x27ca,0", "g7d124slow=7,124,slow,32,0x27ca,0", "g5d124m07c0=5,124,fast,16,0x07c0,0",
                        "g7d124fdt=7,124,fast,16,0x27ca,1", "g9d124slowfdt=9,124,slow,64,0x27ca,1"]
        cfgs = [c for c in a.configs.split(",") if c] if a.configs else default_cfgs
        if a.configs:  # allow ';' separated when commas are inside
            cfgs = [c for c in a.configs.split(";") if c]
        results = []
        for cfg in cfgs:
            name, spec = cfg.split("=")
            parts = spec.split(",")
            gain, dac, rate, integ, mode1804, fdt = parts[:6]
            pol = int(parts[6]) if len(parts) > 6 else 3
            rst.pulse(); s.__init__(w, verbose=False)
            s.rate = rate; s.startup()
            s.state.update(gain=int(gain), dac=int(dac), integration=int(integ), subtract_baseline=0, polarity=pol)
            if int(fdt) == 2: s.fdt_skip = {0x1815}
            if int(fdt): s.configure_fdt()
            s.configure_image(0xA3, force_full=True)
            w.write_word(0x1804, int(mode1804, 16))
            s.mode = 0xA3  # keep configure 'short' so 0x1804 stays as set
            def frames(seconds):
                out = []; t_end = time.monotonic() + seconds
                while time.monotonic() < t_end:
                    r = s.capture(polls=a.polls)
                    if not r["ready"] or not r["alive_after"]:
                        return None
                    out.append(r)
                return out
            print(f"\n=== {name}: pol={pol} gain={gain} dac={dac} rate={rate} integ={integ} 1804={mode1804} fdt={fdt}")
            print("    LIFT finger, taking reference ...", flush=True)
            ref = frames(2.0)
            if not ref: print("    chip stalled"); results.append((name, None)); continue
            n = len(ref[0]["px"]); refpx = [sum(f["px"][i] for f in ref) / len(ref) for i in range(n)]
            noise = sorted(abs(ref[-1]["px"][i] - refpx[i]) for i in range(n))[n // 2]
            print(f"    reference median={ref[-1]['median']} spread={ref[-1]['p95'] - ref[-1]['p5']} frame-to-frame noise={noise:.0f}")
            print("    >>> TOUCH NOW and hold (4 s) <<<", flush=True)
            time.sleep(1.0)
            touched = frames(3.0)
            if not touched: print("    chip stalled"); results.append((name, None)); continue
            best = None
            for f in touched:
                diff = [f["px"][i] - refpx[i] for i in range(n)]
                sd = sorted(diff); med = sd[n // 2]; mad = sorted(abs(d - med) for d in diff)[n // 2]
                score = (abs(med), mad)
                if best is None or score > best[0]: best = (score, f, med, mad)
            (_, f, med, mad) = best
            print(f"    touched: median shift={med:+.0f} ridge-like deviation (MAD)={mad:.0f} vs noise {noise:.0f}  -> {'SIGNAL' if mad > 3 * max(noise, 1) or abs(med) > 8 * max(noise, 1) else 'nothing'}")
            results.append((name, (med, mad, noise)))
            # banding/offset-free ridge estimate: subtract reference, remove per-row median, measure MAD
            W_, H_ = f["width"], f["height"]
            diff = [f["px"][i] - refpx[i] for i in range(n)]
            rowmed = [sorted(diff[r * W_:(r + 1) * W_])[W_ // 2] for r in range(H_)]
            flat = [diff[i] - rowmed[i // W_] for i in range(n)]
            fm = sorted(flat)[n // 2]; fmad = sorted(abs(v - fm) for v in flat)[n // 2]
            clipped = sum(1 for v in f["px"] if v <= 0 or v >= 4092)
            print(f"    row-flattened finger residual MAD={fmad:.0f} (ridge candidate), clipped px={clipped}")
            if a.pgm:
                write_pgm(f"{a.pgm}.{name}.pgm", f)
                import json
                with open(f"{a.pgm}.{name}.json", "w") as jf:
                    json.dump(dict(width=W_, height=H_, ref=refpx, touched=f["px"], all_touched=[t["px"] for t in touched], ref_frames=[t["px"] for t in ref]), jf)
            print("    LIFT finger", flush=True); time.sleep(1.5)
        print("\n=== summary (median shift, ridge MAD, noise)")
        for name, r in results:
            print(f"  {name:18s} {'stalled' if r is None else f'{r[0]:+6.0f} {r[1]:5.0f} {r[2]:4.0f}'}")
        return
    if a.cmd == "fdt-bisect":
        cands = [None, 0x1801, 0x180C, 0x1807, 0x1806, 0x180A, 0x180B, 0x1812, 0x1881, 0x1800, 0x1816, 0x1804, 0x1808, 0x1815, 0x180D, 0x1880, 0x1884, 0x1887, 0x1888, "chan", "1a83"]
        for cand in cands:
            rst.pulse(); s.__init__(w); s.state["integration"] = a.integration; s.state["subtract_baseline"] = 0; s.rate = a.rate; s.startup()
            s.fdt_skip = set() if cand is None else {cand}
            s.configure_fdt()
            r = s.capture(polls=a.polls)
            label = "none" if cand is None else (cand if isinstance(cand, str) else f"{cand:04x}")
            print(f"  skip {label:5s}: median={r['median']:5d} min={r['min']:5d} max={r['max']:5d} alive={r['alive_after']}")
        return
    if a.cmd == "fulldiff":
        def full():
            d = {}
            for a_ in list(range(0x1800, 0x1900)) + list(range(0x1A00, 0x1A20)) + list(range(0x1A80, 0x1A90)):
                try: d[a_] = w.read_word(a_, check_crc=False)
                except IOError: d[a_] = None
            for a_ in range(0x00, 0x100):
                d[0x10000 + a_] = w.read_byte(a_)
            return d
        r = s.capture(polls=a.polls); print(f"  image pre-fdt median={r['median']}")
        d1 = full()
        print("  fdt configure:", s.configure_fdt())
        r = s.capture(polls=a.polls); print(f"  image post-fdt median={r['median']}")
        d2 = full()
        diffs = [(k, d1[k], d2[k]) for k in d1 if d1[k] != d2[k]]
        print("  differing registers (addr: pre -> post), byte regs shown as b:XX")
        for k, x, y in diffs:
            name = f"b:{k - 0x10000:02x}" if k >= 0x10000 else f"{k:04x}"
            print(f"    {name}: {x if x is None else f'{x:04x}'} -> {y if y is None else f'{y:04x}'}")
        return
    if a.cmd == "fdt-steps":
        def cap(label):
            r = s.capture(polls=a.polls); print(f"  {label:28s} configure={r['configure']:22s} median={r['median']} min={r['min']} max={r['max']} alive={r['alive_after']}")
        cap("startup"); cap("startup#2")
        print("  fdt configure:", s.configure_fdt()); cap("after fdt configure"); cap("after fdt configure#2")
        s.fdt_measure(); print("  fdt values", s.fdt_read()); cap("after fdt measure")
        v = s.fdt_read(); base = [max(0, x - 20) for x in v] + [min(1023, x + 20) for x in v]
        s.fdt_arm(base, down=True); cap("after fdt arm")
        s.fdt_arm(base, down=False); cap("after fdt arm up")
        return
    if a.cmd == "hunt2":
        print("  fdt configure:", s.configure_fdt())
        s.fdt_measure(); v0 = s.fdt_read(); print(f"  fdt reference values {v0}")
        best_touch = None; untouched = None; t_end = time.monotonic() + a.timeout; frames = 0; was = False
        while time.monotonic() < t_end:
            s.fdt_measure(); v = s.fdt_read()
            touched = max(abs(x - y) for x, y in zip(v, v0)) > 40
            r = s.capture(polls=a.polls); frames += 1
            if not r["ready"] or not r["alive_after"]:
                print(f"  frame {frames}: ready={r['ready']} alive={r['alive_after']} -- stopping"); break
            spread = r["p95"] - r["p5"]
            if touched != was or frames % 40 == 1:
                print(f"  frame {frames:4d} touched={touched} fdt={v} img median={r['median']} p5={r['p5']} p95={r['p95']} spread={spread}")
            was = touched
            if touched and (best_touch is None or spread > best_touch[0]): best_touch = (spread, r, frames)
            if not touched and untouched is None: untouched = r
        if untouched and a.pgm: write_pgm(a.pgm + ".untouched.pgm", untouched)
        if best_touch:
            spread, r, idx = best_touch
            print(f"  best touched frame #{idx}: spread={spread} min={r['min']} median={r['median']} max={r['max']}")
            if a.pgm: write_pgm(a.pgm + ".touched.pgm", r)
        else:
            print("  no frame with a finger detected")
        return
    if a.cmd == "image":
        last = None
        for i in range(a.n):
            r = s.capture(polls=a.polls, scan_cmd=int(a.scan_cmd, 16), nav=a.nav, start=not a.no_start, peek=a.peek); show(r, f"image#{i}")
            if a.pgm: write_pgm(a.pgm if a.n == 1 else f"{a.pgm}.{i}.pgm", r)
            last = r
        dump_regs(w, "after-image")
        return

    # fdt-image and touch share: baseline image capture first
    if not a.skip_first:
        r = s.capture(polls=a.polls); show(r, "image-before-fdt")
    rimg = dump_regs(w, "image-mode")
    s.configure_fdt()
    ok = s.fdt_measure(); vals = s.fdt_read()
    print(f"  fdt sample: measure_ok={ok} values={vals}")
    # crude down/up bases from one sample: driver uses searched minima +/- margin
    base = [max(0, v - 20) for v in vals] + [min(1023, v + 20) for v in vals]
    rfdt = dump_regs(w, "fdt-mode")
    diff_regs(rimg, rfdt, "image", "fdt")
    s.fdt_arm(base, down=True)
    rarm = dump_regs(w, "fdt-armed")
    diff_regs(rfdt, rarm, "fdt", "armed")
    if a.cmd == "touch":
        wait_irq(a.timeout)
        print(f"  status after IRQ wait: 0x1a82=0x{w.read_word(0x1A82):04x} 0x1881=0x{w.read_word(0x1881):04x}")
    fixes = a.fix.split(",")
    apply_fixes(s, fixes, rimg)
    r = s.capture(polls=a.polls); show(r, f"image-after-fdt fix={a.fix}")
    if a.pgm: write_pgm(a.pgm, r)
    rpost = dump_regs(w, "post")
    diff_regs(rimg, rpost, "image", "post")
    # second capture to see whether the state is sticky
    r = s.capture(polls=a.polls); show(r, f"image-after-fdt#2 fix={a.fix}")
    rst.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Builds the mastercard.device boot ROM.

    pack.py DRIVER_HUNK DIAG OUT16 OUT64

DRIVER_HUNK is the driver linked with -DROM_BUILD, an Amiga executable.
Its hunks are laid out one after another in a single memory block, the
relocations turned into offsets into that block, and the result, followed by
the relocation table, compressed (lz.py) and stored byte-wide at image $0300
(board $0600). DIAG is the DiagArea: diag.s, assembled here with the blob's
board offset, or diag.o, already assembled with BLOBX=1536. It is stored
nibble-wide at image $0040.
OUT16 is the 16 KB image the card shows; OUT64 is the 27C512 image, the
16 KB in all four quarters.

The image holds only our own code: autoconfig data, our DiagArea, our driver.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

import lz

AS = "/opt/amiga/bin/m68k-amigaos-as"
NM = "/opt/amiga/bin/m68k-amigaos-nm"
ROM = 0x4000
DA = 0x40
BLOB = 0x300

# Expansion ROM: the card's own identity, so the driver finds its board.
ER_TYPE = 0xD1          # Zorro II, DiagArea valid, 64 KB
ER_PRODUCT = 0x21
ER_FLAGS = 0x40
ER_MANUFACTURER = 0x0890
ER_DIAGVEC = 0x0080     # board offset of the DiagArea


def fail(msg):
    sys.exit("pack: " + msg)


def hunks(path):
    """Returns ([(memory bytes, data bytes)], [(hunk, offset, target hunk)], symbols)."""
    b = open(path, "rb").read()
    i = 0

    def L():
        nonlocal i
        v = struct.unpack(">L", b[i:i + 4])[0]
        i += 4
        return v
    if L() != 0x3F3:
        fail("driver is not an Amiga executable")
    while L():
        pass
    L()
    first, last = L(), L()
    sizes = [(L() & 0x3FFFFFFF) * 4 for _ in range(last - first + 1)]
    data = [b""] * len(sizes)
    relocs, syms = [], {}
    h = 0
    while i < len(b):
        t = L() & 0x3FFFFFFF
        if t in (0x3E9, 0x3EA):
            n = L()
            data[h] = b[i:i + n * 4]
            i += n * 4
        elif t == 0x3EB:
            L()
        elif t == 0x3EC:
            while True:
                n = L()
                if not n:
                    break
                th = L()
                for _ in range(n):
                    relocs.append((h, L(), th))
        elif t == 0x3F0:
            while True:
                n = L()
                if not n:
                    break
                name = b[i:i + n * 4].rstrip(b"\0").decode()
                i += n * 4
                syms[name] = (h, L())
        elif t == 0x3F1:
            n = L()
            i += n * 4
        elif t == 0x3F2:
            h += 1
        else:
            fail("unsupported hunk type %x in the driver" % t)
    return list(zip(sizes, data)), relocs, syms


def blob(path):
    hs, relocs, syms = hunks(path)
    base, off = [], 0
    for size, _ in hs:
        base.append(off)
        off += (size + 3) & ~3
    total = off
    # Code and data are stored; a trailing bss hunk is just memory.
    stored = bytearray()
    for k, (size, data) in enumerate(hs):
        if any(len(d) for _, d in hs[k:]):
            stored += data.ljust((size + 3) & ~3, b"\0")
    img = bytearray(stored)
    fix = []
    for h, o, th in relocs:
        p = base[h] + o
        if p + 4 > len(img):
            fail("relocation outside the stored image")
        v = struct.unpack(">L", img[p:p + 4])[0] + base[th]
        img[p:p + 4] = struct.pack(">L", v)
        fix.append(p)
    tag = img.find(b"\x4a\xfc")
    while tag >= 0 and struct.unpack(">L", img[tag + 2:tag + 6])[0] != tag:
        tag = img.find(b"\x4a\xfc", tag + 2)
    if tag < 0:
        fail("no RomTag in the driver")
    if total > 0xFFFF:
        fail("driver is over 64 KB, too big for 16-bit relocation offsets")
    data = bytes(img) + b"".join(struct.pack(">H", p) for p in sorted(fix))
    packed = lz.compress(data)
    if lz.decompress(packed, len(data)) != data:
        fail("compression does not round-trip")
    # The memory block also holds the relocation table while it is used;
    # DiagPoint clears it after, for the bss.
    mem = (max(total, len(data)) + 3) & ~3
    out = struct.pack(">4L", mem, len(data), len(fix), tag) + packed
    return out, total, len(fix), len(data)


def diagarea(src, blobx):
    if src.endswith(".o"):
        # Assembled already (build.sh does it with the ROM's toolchain),
        # with BLOBX set to 2 * BLOB.
        b = open(src, "rb").read()
    else:
        with tempfile.TemporaryDirectory() as d:
            o = os.path.join(d, "diag.o")
            r = subprocess.run([AS, "-m68020", "--defsym", "BLOBX=%d" % blobx, "-o", o, src],
                               capture_output=True, text=True)
            if r.returncode:
                fail("diag.s:\n" + r.stderr)
            b = open(o, "rb").read()
    # the code hunk of the object
    i = 0
    while i < len(b):
        t = struct.unpack(">L", b[i:i + 4])[0] & 0x3FFFFFFF
        i += 4
        n = struct.unpack(">L", b[i:i + 4])[0]
        i += 4
        if t == 0x3E9:
            code = b[i:i + n * 4]
            break
        if t in (0x3E7, 0x3E8):
            i += n * 4
    size = struct.unpack(">H", code[2:4])[0]
    return code[:size]


def autoconfig():
    er = bytearray(32)
    er[0], er[1], er[2] = ER_TYPE, ER_PRODUCT, ER_FLAGS
    er[4:6] = struct.pack(">H", ER_MANUFACTURER)
    er[10:12] = struct.pack(">H", ER_DIAGVEC)
    out = bytearray()
    for k, v in enumerate(er):
        if k:
            v ^= 0xFF
        out += bytes([(v & 0xF0) | 0x0F, (v << 4 & 0xF0) | 0x0F])
    return out


def main():
    if len(sys.argv) != 5:
        fail("usage: pack.py DRIVER_HUNK DIAG_S OUT16 OUT64")
    drv, dsrc, out16, out64 = sys.argv[1:]
    b, total, nfix, raw = blob(drv)
    da = diagarea(dsrc, 2 * BLOB)
    if DA + 2 * len(da) > BLOB:
        fail("DiagArea is %d bytes, room for %d" % (len(da), (BLOB - DA) // 2))
    if BLOB + len(b) > ROM:
        fail("driver blob is %d bytes too big" % (BLOB + len(b) - ROM))
    img = bytearray(ROM)
    img[0:0x40] = autoconfig()
    for k, v in enumerate(da):
        img[DA + 2 * k] = (v & 0xF0) | 0x0F
        img[DA + 2 * k + 1] = (v << 4 & 0xF0) | 0x0F
    img[BLOB:BLOB + len(b)] = b
    open(out16, "wb").write(img)
    open(out64, "wb").write(bytes(img) * 4)
    print("DiagArea %d bytes, driver %d bytes in memory, %d bytes with %d relocations packed to %d, %d bytes free" %
          (len(da), total, raw, nfix, len(b), ROM - BLOB - len(b)))
    for p in (out16, out64):
        print("%s sha1 %s" % (os.path.basename(p), hashlib.sha1(open(p, "rb").read()).hexdigest()))


main()

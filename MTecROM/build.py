#!/usr/bin/env python3
"""Builds the MasterCard boot ROM from source.

    build.py ORIGINAL PATCH_S OUTDIR

ORIGINAL is the card's ROM (16 KB image or 64 KB 27C512 read), used to
generate the source with dis2src.py when OUTDIR/src is missing, and to check
the build. PATCH_S is MTecPatch.c compiled to assembler with ROM_BUILD.

First the driver and DiagArea source are built alone, with the original copy
length and init routine, and must give the original ROM byte for byte. That
proves the source, the nibble packing and the autoconfig data are complete.
Then the patch is assembled right after the driver, in the same unit, so it
calls the driver's init by name, and the DiagArea gets the new copy length and
the patch's entry as the init routine. Writes OUTDIR/MTecROM-16k.bin for the
emulator and OUTDIR/MTecROM-27C512.bin, the image in all four quarters.
"""
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = "/opt/amiga/bin/"
AS = BIN + "m68k-amigaos-as"
NM = BIN + "m68k-amigaos-nm"

ROM = 0x4000
DA = 0x40               # DiagArea, nibble-wide, image $0040-$01FF
DA_LEN = 0xE0
DRIVER = 0x200          # driver, byte-wide, copied to RAM by DiagPoint
ORIG_COPY_LEN = 0x23E0

# Expansion ROM fields, from the original ROM.
ER_TYPE = 0xD1          # Zorro II, DiagArea valid, 64 KB
ER_PRODUCT = 0x21
ER_FLAGS = 0x40
ER_MANUFACTURER = 0x0890
ER_SERIAL = 0
ER_DIAGVEC = 0x0080     # board offset of the DiagArea


def fail(msg):
    sys.exit("build: " + msg)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        fail("%s failed:\n%s%s" % (os.path.basename(cmd[0]), r.stdout, r.stderr))
    return r.stdout


def obj_code(path, relocs_ok=False):
    """The code of a one-hunk object with no relocations or external references.
    relocs_ok allows relocations to its own hunk, whose values are then offsets."""
    b = open(path, "rb").read()
    i, code, hunks = 0, None, 0

    def L():
        nonlocal i
        v = struct.unpack(">L", b[i:i + 4])[0]
        i += 4
        return v
    while i < len(b):
        t = L() & 0x3FFFFFFF
        if t in (0x3E7, 0x3E8):         # unit, name
            n = L()
            i += n * 4
        elif t == 0x3E9:
            hunks += 1
            n = L()
            code = b[i:i + n * 4]
            i += n * 4
        elif t in (0x3EA, 0x3EB):
            n = L()
            if n:
                fail("%s has a data or bss hunk, which the ROM can't hold" % path)
            hunks += 1
        elif t in (0x3EC, 0x3ED, 0x3EE, 0x3F7, 0x3F8):
            while True:                 # an empty table is harmless
                n = L()
                if not n:
                    break
                if not relocs_ok or t != 0x3EC or L() != 0:
                    fail("%s has relocations, so it would not run from the copy" % path)
                i += n * 4
        elif t == 0x3EF:
            while True:
                h = L()
                if not h:
                    break
                kind, n = h >> 24, h & 0xFFFFFF
                name = b[i:i + n * 4].rstrip(b"\0").decode()
                i += n * 4
                if kind < 0x80:         # definition: value follows
                    i += 4
                else:
                    fail("%s has an unresolved reference to %s" % (path, name))
        elif t == 0x3F0:
            while True:
                n = L()
                if not n:
                    break
                i += n * 4 + 4
        elif t == 0x3F1:
            n = L()
            i += n * 4
        elif t == 0x3F2:
            pass
        else:
            fail("unexpected hunk type %x in %s" % (t, path))
    if code is None or hunks != 1:
        fail("%s must have exactly one code hunk" % path)
    return code


def assemble(src_files, obj, defsyms=(), relocs_ok=False):
    cat = obj[:-2] + ".s"
    with open(cat, "w") as f:
        for s in src_files:
            f.write(open(s).read())
            f.write("\n")
    cmd = [AS, "-m68020", "-o", obj, cat]
    for k, v in defsyms:
        cmd += ["--defsym", "%s=%d" % (k, v)]
    run(cmd)
    syms = {}
    for line in run([NM, obj]).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return obj_code(obj, relocs_ok), syms


def autoconfig():
    """The expansion ROM nibbles at image $0000-$003F: high nibble of each
    byte, low nibble F, inverted except er_Type."""
    er = bytearray(32)
    er[0], er[1], er[2] = ER_TYPE, ER_PRODUCT, ER_FLAGS
    er[4:6] = struct.pack(">H", ER_MANUFACTURER)
    er[6:10] = struct.pack(">L", ER_SERIAL)
    er[10:12] = struct.pack(">H", ER_DIAGVEC)
    out = bytearray()
    for k, v in enumerate(er):
        if k:
            v ^= 0xFF
        out += bytes([(v & 0xF0) | 0x0F, (v << 4 & 0xF0) | 0x0F])
    return out


def pack(driver, diag):
    """Lays out the 16 KB image the card shows."""
    if len(diag) < DA_LEN:
        diag += bytes(DA_LEN - len(diag))
    if len(diag) != DA_LEN:
        fail("DiagArea is %d bytes, not %d" % (len(diag), DA_LEN))
    if DRIVER + len(driver) > ROM:
        fail("driver and patch are %d bytes too big" % (DRIVER + len(driver) - ROM))
    img = bytearray(ROM)
    img[0:0x40] = autoconfig()
    size = diag[3]                      # da_Size
    for k, v in enumerate(diag):
        # M-Tec left the DiagArea's unused header bytes and its tail blank.
        if v == 0 and (0x10 <= k < 0x20 or k >= size):
            continue
        img[DA + 2 * k] = (v & 0xF0) | 0x0F
        img[DA + 2 * k + 1] = (v << 4 & 0xF0) | 0x0F
    img[DRIVER:DRIVER + len(driver)] = driver
    return img


def build(srcdir, outdir, patch_s, name):
    """Assembles driver (+ patch) and DiagArea; returns the image and the driver's symbols."""
    files = [os.path.join(srcdir, "driver.s")] + ([patch_s] if patch_s else [])
    driver, syms = assemble(files, os.path.join(outdir, name + "-driver.o"))
    init = "_rom_entry" if patch_s else "_mtec_init"
    for s in ("functable", "datatable", "drv_name", "drv_idstring", init):
        if s not in syms:
            fail("symbol %s missing from the driver" % s)
    copy_len = (len(driver) + 15) & ~15
    if not patch_s:
        copy_len = ORIG_COPY_LEN
    if len(driver) > copy_len:
        fail("driver is longer than the copy length")
    driver += bytes(copy_len - len(driver))
    diag, dsyms = assemble([os.path.join(srcdir, "diag.s")], os.path.join(outdir, name + "-diag.o"), [
        ("DRIVER_COPY_LEN", copy_len),
        ("DRV_NAME", syms["drv_name"]),
        ("DRV_IDSTRING", syms["drv_idstring"]),
        ("DRV_FUNCTABLE", syms["functable"]),
        ("DRV_DATATABLE", syms["datatable"]),
        ("DRV_INIT", syms[init])], relocs_ok=True)  # rt_Init is a DiagArea offset
    return pack(driver, diag), syms, copy_len


def main():
    if len(sys.argv) != 4:
        fail("usage: build.py ORIGINAL PATCH_S OUTDIR")
    orig, patch_s, outdir = sys.argv[1:]
    srcdir = os.path.join(outdir, "src")
    raw = open(orig, "rb").read()
    if len(raw) == 0x10000:
        raw = raw[:ROM]
    if len(raw) != ROM:
        fail("original must be 16 KB or 64 KB")
    if not os.path.exists(os.path.join(srcdir, "driver.s")):
        subprocess.run([sys.executable, os.path.join(HERE, "dis2src.py"), orig, srcdir], check=True)

    img, _, _ = build(srcdir, outdir, None, "orig")
    if img != raw:
        k = next(k for k in range(ROM) if img[k] != raw[k])
        fail("source build without the patch differs from the original at image $%04X" % k)
    print("source without the patch rebuilds the original ROM exactly")

    img, syms, copy_len = build(srcdir, outdir, patch_s, "patched")
    print("driver $0000-$%04X, patch from $%04X, copy length $%04X, %d bytes free" %
          (ORIG_COPY_LEN, ORIG_COPY_LEN, copy_len, ROM - DRIVER - copy_len))
    print("init routine: _rom_entry at driver $%04X" % syms["_rom_entry"])
    out16 = os.path.join(outdir, "MTecROM-16k.bin")
    out64 = os.path.join(outdir, "MTecROM-27C512.bin")
    open(out16, "wb").write(img)
    open(out64, "wb").write(bytes(img) * 4)
    for p in (out16, out64):
        print("%s sha1 %s" % (os.path.basename(p), hashlib.sha1(open(p, "rb").read()).hexdigest()))


main()

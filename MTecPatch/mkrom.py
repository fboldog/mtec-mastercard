#!/usr/bin/env python3
"""Builds a MasterCard boot ROM with MTecPatch inside.

    mkrom.py ORIGINAL BLOB OUT16 OUT64

ORIGINAL is the card's ROM: the 16 KB image, or the full 64 KB 27C512 read.
BLOB is the ROM_BUILD hunk file of MTecPatch.c. OUT16 is the 16 KB image the
card shows, for the emulator. OUT64 is the 27C512 image to program: the image
in all four 16 KB quarters. The socket ties A14 and A15 to ground, so only
$0000 is read, but filling all four makes the chip work in any wiring.

The ROM loader in the DiagArea copies COPY_LEN bytes of driver from image
$0200 to RAM and fixes up a RomTag whose init routine is driver offset $40.
The blob goes right after the driver, the copy length grows to cover it, and
the init routine becomes the blob's rom_entry, which calls the old one first.
"""
import hashlib
import struct
import sys

DRIVER = 0x200          # driver start in the image
COPY_LEN = 0x23E0       # bytes the original loader copies
OLD_INIT = 0x40         # driver offset of the original init routine
DA = 0x40               # DiagArea start in the image, nibble-wide
DA_COPY_LEN = 0x50      # DiagArea offset of the copy length in move.l #imm,d0
DA_INIT = 0xDA          # DiagArea offset of the init pointer in the autoinit table
DA_SIZE = 0xDE
ORIGINAL_SHA1 = "eea5e9abb3ffb4eddf09825c7e8de75d0d08c01f"   # 64 KB chip read


def fail(msg):
    sys.exit("mkrom: " + msg)


def code_hunk(path):
    """Returns the only code hunk of an Amiga executable, which must have no relocations."""
    b = open(path, "rb").read()
    i = 0

    def L():
        nonlocal i
        v = struct.unpack(">L", b[i:i + 4])[0]
        i += 4
        return v
    if L() != 0x3F3:
        fail("blob is not an Amiga executable")
    while L():
        pass
    L()                                 # table size
    first, last = L(), L()
    if last != first:
        fail("blob has more than one hunk")
    L()                                 # hunk size
    code = None
    while i < len(b):
        t = L() & 0x3FFFFFFF
        if t == 0x3E9:
            n = L()
            code = b[i:i + n * 4]
            i += n * 4
        elif t in (0x3EC, 0x3F7, 0x3F8):
            fail("blob has relocations, so it would not run from the copy")
        elif t == 0x3F0:
            while True:
                n = L()
                if not n:
                    break
                i += n * 4 + 4
        elif t == 0x3F2:
            pass
        else:
            fail("unexpected hunk type %x in blob" % t)
    if code is None:
        fail("blob has no code")
    return code


def da_get(img, off, n):
    return bytes(((img[DA + 2 * (off + k)] >> 4) << 4) | (img[DA + 2 * (off + k) + 1] >> 4)
                 for k in range(n))


def da_put(img, off, data):
    for k, v in enumerate(data):
        p = DA + 2 * (off + k)
        img[p] = (v & 0xF0) | (img[p] & 0x0F)
        img[p + 1] = ((v & 0x0F) << 4) | (img[p + 1] & 0x0F)


def main():
    if len(sys.argv) != 5:
        fail(__doc__.strip().splitlines()[2].strip())
    orig, blob_path, out16, out64 = sys.argv[1:]
    raw = open(orig, "rb").read()
    if len(raw) == 0x10000:
        if hashlib.sha1(raw).hexdigest() != ORIGINAL_SHA1:
            print("mkrom: warning, this is not the chip read this was written for")
        if raw[0x8000:0xC000] != raw[:0x4000] or any(raw[0x4000:0x8000]) or any(raw[0xC000:]):
            fail("64 KB image does not have the expected layout")
        raw = raw[:0x4000]
    if len(raw) != 0x4000:
        fail("original must be 16 KB or 64 KB")
    img = bytearray(raw)

    # Check every value this changes, so a different ROM is refused.
    if da_get(img, 0, 4) != bytes([0x10, 0x00, 0x00, DA_SIZE]):
        fail("DiagArea header not as expected")
    if da_get(img, DA_COPY_LEN - 2, 6) != b"\x20\x3c" + struct.pack(">L", COPY_LEN):
        fail("copy length not found in the loader")
    if da_get(img, DA_INIT, 4) != struct.pack(">L", OLD_INIT):
        fail("init pointer not found in the autoinit table")
    if any(img[DRIVER + COPY_LEN:]):
        fail("space after the driver is not empty")
    for off in list(range(DA_COPY_LEN, DA_COPY_LEN + 4)) + list(range(DA_INIT, DA_INIT + 4)):
        if img[DA + 2 * off] & 0x0F != 0x0F or img[DA + 2 * off + 1] & 0x0F != 0x0F:
            fail("DiagArea is not nibble-wide where expected")

    blob = code_hunk(blob_path)
    if blob[:4] != bytes.fromhex("43fa001a"):
        fail("blob does not start with rom_entry")
    new_len = (COPY_LEN + len(blob) + 15) & ~15
    if DRIVER + new_len > 0x4000:
        fail("blob is %d bytes, %d too many" % (len(blob), DRIVER + new_len - 0x4000))
    img[DRIVER + COPY_LEN:DRIVER + COPY_LEN + len(blob)] = blob
    da_put(img, DA_COPY_LEN, struct.pack(">L", new_len))
    da_put(img, DA_INIT, struct.pack(">L", COPY_LEN))

    if da_get(img, DA_COPY_LEN, 4) != struct.pack(">L", new_len) or \
            da_get(img, DA_INIT, 4) != struct.pack(">L", COPY_LEN):
        fail("DiagArea write check failed")
    changed = [k for k in range(DA, DRIVER) if img[k] != raw[k]]
    print("blob %d bytes at image $%04X, copy length $%04X, %d bytes free" %
          (len(blob), DRIVER + COPY_LEN, new_len, 0x4000 - DRIVER - new_len))
    print("DiagArea bytes changed at image", " ".join("$%04X" % k for k in changed))

    open(out16, "wb").write(img)
    open(out64, "wb").write(bytes(img) * 4)
    print("%s sha1 %s" % (out16, hashlib.sha1(img).hexdigest()))
    print("%s sha1 %s" % (out64, hashlib.sha1(open(out64, "rb").read()).hexdigest()))


main()

"""The boot ROM's compression: ZX0 (V2), made by salvador.

compress() runs salvador (external/salvador, built by build.sh into
gen/salvador). decompress() is a bit-for-bit model of the decoder in diag.s,
which is salvador's asm/68000/unzx0_68000.S: pack.py checks every image with
it. Like the original, the decoder refills its bit queue only when it reads
the stop bit of an Elias gamma number, not for flags or data bits; the model
does the same, so an image that relied on anything else would fail here.
"""
import os
import subprocess
import tempfile

SALVADOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gen", "salvador")


def compress(data):
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in"), os.path.join(d, "out")
        open(src, "wb").write(data)
        r = subprocess.run([SALVADOR, src, dst], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("salvador: " + (r.stderr or r.stdout))
        return open(dst, "rb").read()


def decompress(src):
    """unzx0_68000.S, register by register: d1 the bit queue, d2 the negative
    last offset, d0 the number being read."""
    out = bytearray()
    q = {"d1": 0x80, "a0": 0}

    def add():                              # add.b d1,d1
        r = q["d1"] << 1
        q["d1"] = r & 0xFF
        return r >> 8, (r & 0xFF) == 0

    def elias(d0, data_first=False):
        if data_first:                      # entered at .elias_bt
            c, _ = add()
            d0 = (d0 << 1 | c) & 0xFFFFFFFF
        while True:
            c, z = add()                    # .elias_loop
            if z:                           # move.b (a0)+,d1 ; addx.b d1,d1
                r = src[q["a0"]] << 1 | c
                q["a0"] += 1
                q["d1"] = r & 0xFF
                c = r >> 8
            if c:
                return d0
            c, _ = add()                    # .elias_bt
            d0 = (d0 << 1 | c) & 0xFFFFFFFF

    def copy(neg, n):
        p = len(out) + neg
        for _ in range(n):
            out.append(out[p])
            p += 1

    d2 = -1
    literals = True
    while True:
        if literals:
            n = elias(1)
            out += src[q["a0"]:q["a0"] + n]
            q["a0"] += n
            c, _ = add()
            if not c:                       # the last offset again
                copy(d2, elias(1))
                c, _ = add()
                if not c:
                    continue
        d0 = (elias(0xFFFFFFFE) + 1) & 0xFF # .get_offset
        if not d0:
            return bytes(out)
        w = (d0 - 256) << 8 | src[q["a0"]]
        q["a0"] += 1
        n = 1 if w & 1 else elias(1, True)
        d2 = w >> 1
        copy(d2, n + 1)
        c, _ = add()
        literals = not c

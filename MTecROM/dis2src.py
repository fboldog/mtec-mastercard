#!/usr/bin/env python3
"""Turns the MasterCard boot ROM into assembler source that rebuilds it.

    dis2src.py ROM OUTDIR

ROM is the 16 KB image or the 64 KB 27C512 read. Writes OUTDIR/driver.s (the
mtecscsi.device code, byte-wide at image $0200) and OUTDIR/diag.s (the DiagArea
loader, nibble-wide at image $0040). The source is M-Tec's code, so it is
generated from the user's own ROM and not kept in git.

Code is found by following control flow from the known entry points and those
in symbols.txt. Each instruction is checked by assembling the result: one that
GNU as does not rebuild to the same bytes is written as data, with the
instruction as a comment. Everything not reached as code is written as data.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = "/opt/amiga/bin/"
OBJDUMP = BIN + "m68k-amigaos-objdump"
AS = BIN + "m68k-amigaos-as"
NM = BIN + "m68k-amigaos-nm"

DRIVER = 0x200
DRIVER_LEN = 0x23E0
DA = 0x40
DA_LEN = 0xE0           # nibble-decoded bytes, image $0040-$01FF


def fail(msg):
    sys.exit("dis2src: " + msg)


def load_rom(path):
    raw = open(path, "rb").read()
    if len(raw) == 0x10000:
        raw = raw[:0x4000]
    if len(raw) != 0x4000:
        fail("ROM must be 16 KB or 64 KB")
    return raw


def nibbles(img, start, n):
    return bytes(((img[start + 2 * k] >> 4) << 4) | (img[start + 2 * k + 1] >> 4) for k in range(n))


def load_symbols(path):
    """symbols.txt: 'section addr kind name [comment]', kind = code, data or label."""
    syms = {"driver": {}, "diag": {}}
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        sec, addr, kind, name = line.split()[:4]
        syms[sec][int(addr, 16)] = (kind, name)
    return syms


class Insn:
    def __init__(self, addr, data, text):
        self.addr, self.data, self.text = addr, data, text

    @property
    def end(self):
        return self.addr + len(self.data)


def objdump(blob, start, stop):
    """Decodes blob[start:stop] linearly; returns a list of Insn."""
    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        f.write(blob)
        f.flush()
        out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "m68k:68020",
                              "--start-address=%d" % start, "--stop-address=%d" % stop, f.name],
                             capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*(?:\t(.*))?$", line)
        if not m:
            continue
        addr, hexs, text = int(m.group(1), 16), m.group(2).replace(" ", ""), m.group(3)
        if text is None:                # continuation of a long instruction
            if res:
                res[-1].data += bytes.fromhex(hexs)
            continue
        res.append(Insn(addr, bytes.fromhex(hexs), text.strip()))
    return res


BRANCH = re.compile(r"^(b(?:ra|sr|hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)\.[swl]|db\w+|jsr|jmp|bsr\.[swl])\b")
STOP = re.compile(r"^(rts|rte|rtr|rtd|bra\.[swl]|jmp|illegal|\.short|\.word)\b")
PCREF = re.compile(r"0x([0-9a-f]+)\(pc\)")
PCIDX = re.compile(r"\(0x([0-9a-f]+),pc,")
TARGET = re.compile(r"(?:^|,)0x([0-9a-f]+)$")


def find_code(blob, seeds, limit):
    """Follows control flow from seeds; returns {addr: Insn} and data references."""
    code, refs = {}, set()
    work = sorted(set(seeds))
    while work:
        a = work.pop()
        while a < limit and a not in code:
            ins = objdump(blob, a, min(a + 10, limit))
            if not ins or ins[0].addr != a:
                break
            i = ins[0]
            if i.text.startswith(".short") or i.end > limit:
                break
            code[a] = i
            op = i.text.split()[0]
            args = i.text[len(op):].strip()
            if BRANCH.match(i.text):
                m = TARGET.search(args)
                if m and "(" not in args:
                    work.append(int(m.group(1), 16))
                m = PCREF.search(args)
                if m and op in ("jsr", "jmp"):
                    work.append(int(m.group(1), 16))
            else:
                for m in list(PCREF.finditer(args)) + list(PCIDX.finditer(args)):
                    refs.add(int(m.group(1), 16))
            if STOP.match(i.text):
                break
            a = i.end
    return code, refs


def label(names, a):
    return names.get(a) or "L%04x" % a


def fix_inner(labels, code, names):
    """A target inside an instruction can't take a label: use label+offset."""
    starts = sorted(code)
    import bisect
    inner = {}
    for a in list(labels):
        j = bisect.bisect_right(starts, a) - 1
        if j >= 0 and starts[j] < a < code[starts[j]].end:
            inner[a] = starts[j]
    return inner


def symbolise(i, names, labels):
    """Rewrites addresses in an instruction to labels."""
    op = i.text.split()[0]
    args = i.text[len(op):].strip()

    def pcref(m):
        a = int(m.group(1), 16)
        labels.add(a)
        return label(names, a) + "(pc)"
    args = PCREF.sub(pcref, args)

    def pcidx(m):
        a = int(m.group(1), 16)
        labels.add(a)
        return "(%s,pc," % label(names, a)
    args = PCIDX.sub(pcidx, args)
    if BRANCH.match(i.text) and "(" not in args:
        m = TARGET.search(args)
        if m:
            a = int(m.group(1), 16)
            labels.add(a)
            args = args[:m.start(1) - 2] + label(names, a)
    return ("%s %s" % (op, args)).strip()


def absw_variants(text):
    """GNU as reads 0x4 as absolute long; the original may be absolute short."""
    hits = list(re.finditer(r"(?<![\w(#.])(0x[0-9a-f]+)(?![\w(.])", text))
    out = []
    for n in range(1, 1 << len(hits)):
        t = text
        for k in reversed(range(len(hits))):
            if n >> k & 1:
                h = hits[k]
                t = t[:h.end()] + ".w" + t[h.end():]
        out.append(t)
    return out


def assemble(src):
    """Assembles src and returns (code bytes, {symbol: value})."""
    with tempfile.TemporaryDirectory() as d:
        s, o = os.path.join(d, "t.s"), os.path.join(d, "t.o")
        open(s, "w").write(src)
        r = subprocess.run([AS, "-m68020", "-o", o, s], capture_output=True, text=True)
        if r.returncode:
            return None, r.stderr
        nm = subprocess.run([NM, o], capture_output=True, text=True, check=True).stdout
        syms = {}
        for line in nm.splitlines():
            p = line.split()
            if len(p) == 3:
                syms[p[2]] = int(p[0], 16)
        # Amiga hunk object: pull the code hunk out.
        b = open(o, "rb").read()
    return hunk_code(b), syms


def hunk_code(b):
    import struct
    i = 0

    def L():
        nonlocal i
        v = struct.unpack(">L", b[i:i + 4])[0]
        i += 4
        return v
    if L() != 0x3E7:
        fail("unexpected object format")
    n = L()
    i += n * 4
    while i < len(b):
        t = L() & 0x3FFFFFFF
        if t == 0x3E9:
            n = L()
            return b[i:i + n * 4]
        elif t in (0x3EA, 0x3EB):
            n = L()
            i += n * 4 if t == 0x3EA else 0
        else:
            n = L()
            i += n * 4
    return b""


def data_lines(blob, a, b):
    """Data from a to b, as .ascii for text runs and .byte otherwise."""
    out = []
    while a < b:
        s = a
        while s < b and (32 <= blob[s] < 127 or blob[s] in (9, 10, 13)):
            s += 1
        if s - a >= 4:
            txt = blob[a:s].decode("latin-1")
            txt = txt.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
            out.append('\t.ascii "%s"' % txt)
            a = s
            continue
        e = a
        while e < b and e - a < 16 and not (e > a and sum(1 for k in range(e, min(e + 4, b))
                                                          if 32 <= blob[k] < 127) == 4):
            e += 1
        out.append("\t.byte " + ",".join("0x%02x" % x for x in blob[a:e]))
        a = e
    return out


def emit(blob, code, raw, forms, names, comments, refs, tables, prefix):
    labels = set(refs) | set(names)
    texts = {}
    for s in sorted(code):
        if s not in raw:
            texts[s] = forms.get(s) or symbolise(code[s], names, labels)
    tabs = {}
    for k, (size, base) in tables.items():
        tgt = table_target(blob, k, size, base)
        labels.update((base, tgt))
        tabs[k] = size, "\t.%s %s-%s" % ("short" if size == 2 else "long", label(names, tgt), label(names, base))
    live = {s: code[s] for s in code if s not in raw}
    for a, st in fix_inner(labels, live, names).items():
        labels.discard(a)
        labels.add(st)
        old, new = label(names, a), "%s+%d" % (label(names, st), a - st)
        pat = re.compile(r"\b%s\b" % re.escape(old))
        for k in texts:
            texts[k] = pat.sub(new, texts[k])
        for k in tabs:
            tabs[k] = tabs[k][0], pat.sub(new, tabs[k][1])
    cuts = set(labels) | set(live) | set(tabs)
    lines, marks, a = [], [], 0
    while a < len(blob):
        if a in labels:
            lines.append("%s:" % label(names, a))
        if a in comments:
            lines.append("\t| " + comments[a])
        if a in tabs:
            lines.append(tabs[a][1])
            a += tabs[a][0]
            continue
        if a in code and a not in raw:
            marks.append(a)
            lines.append("%s_%04x:" % (prefix, a))
            lines.append("\t%s" % texts[a])
            a = code[a].end
            continue
        e = a + 1
        while e < len(blob) and e not in cuts:
            e += 1
        if a in code:                   # forced to data: keep the instruction as a comment
            e = code[a].end
            lines.append("\t| %s" % code[a].text)
        lines.extend(data_lines(blob, a, e))
        a = e
    return lines, marks, texts


def table_target(blob, k, size, base):
    v = int.from_bytes(blob[k:k + size], "big", signed=True)
    return base + v


def generate(blob, seeds, names, comments, tables, prefix):
    """Returns assembler source for blob, verified to rebuild it exactly."""
    code, refs = find_code(blob, seeds, len(blob))
    raw, forms = set(), {}
    for _ in range(3000):
        lines, marks, texts = emit(blob, code, raw, forms, names, comments, refs, tables, prefix)
        src = "\n".join(lines) + "\n"
        out, syms = assemble(src)
        if out is None:
            m = re.search(r":(\d+): Error", syms)
            if not m:
                fail("assembler failed:\n" + syms)
            m2 = re.match(r"%s_([0-9a-f]{4}):" % prefix, lines[int(m.group(1)) - 2])
            if not m2:
                fail("assembler error:\n" + syms)
            raw.add(int(m2.group(1), 16))
            continue
        # Sizes first: a wrong size moves everything after it, and the
        # displacements of earlier instructions that point past it.
        prev, moved = None, False
        for s in marks:
            if syms.get("%s_%04x" % (prefix, s)) != s:
                raw.add(prev)           # the instruction before changed size
                moved = True
                break
            prev = s
        if moved:
            continue
        bad = [s for s in marks if out[s:s + len(code[s].data)] != code[s].data]
        if not bad:
            if out[:len(blob)] != blob or any(out[len(blob):]):
                k = next(k for k in range(len(blob)) if out[k] != blob[k])
                fail("output differs at %x outside instructions" % k)
            return src, code, raw
        for s in bad:
            i, v = code[s], None
            if s not in forms and texts[s] == i.text:
                for t in absw_variants(i.text):
                    o2, _ = assemble("\t%s\n" % t)
                    if o2 is not None and o2[:len(i.data)] == i.data:
                        v = t
                        break
            if v:
                forms[s] = v
            else:
                raw.add(s)
    fail("did not converge")


def seeds_tables(blob, sym):
    """Entry points and offset tables from one section of symbols.txt."""
    names = {a: n for a, (k, n) in sym.items()}
    seeds = [a for a, (k, n) in sym.items() if k == "code"]
    tables = {}                         # entry offset: (size, base)
    for a, (k, n) in sym.items():
        if k == "wtable":               # -1, word offsets from the table start, -1
            e = a + 2
            while blob[e:e + 2] != b"\xff\xff":
                tables[e] = (2, a)
                e += 2
        elif k.startswith("wtable"):    # wtableN: N word offsets from the table start
            for e in range(a, a + 2 * int(k[6:]), 2):
                tables[e] = (2, a)
        elif k.startswith("rtable"):    # rtableN:size:bias, each relative to itself + bias
            n, size, bias = k[6:].split(":")
            for j in range(int(n)):
                e = a + j * int(size)
                tables[e] = (int(size), e + int(bias, 0))
    for e, (size, base) in tables.items():
        seeds.append(table_target(blob, e, size, base))
    return names, seeds, tables


def main():
    if len(sys.argv) != 3:
        fail("usage: dis2src.py ROM OUTDIR")
    img = load_rom(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    syms = load_symbols(os.path.join(HERE, "symbols.txt"))

    hdr = ("| Generated by dis2src.py from %s. Do not edit, and do not publish:\n"
           "| this is M-Tec's code. Rebuilds the original bytes exactly.\n" % os.path.basename(sys.argv[1]))
    for sec, blob in (("driver", img[DRIVER:DRIVER + DRIVER_LEN]), ("diag", nibbles(img, DA, DA_LEN))):
        names, seeds, tables = seeds_tables(blob, syms[sec])
        src, code, raw = generate(blob, seeds, names, {}, tables, "I" if sec == "driver" else "D")
        src = re.sub(r"^[ID]_[0-9a-f]{4}:\n", "", src, flags=re.M)
        if sec == "diag":
            src = symbolic_diag(src)
        open(os.path.join(outdir, sec + ".s"), "w").write(hdr + src)
        ncode = sum(len(code[a].data) for a in code if a not in raw)
        print("%s: %d bytes, %d as instructions, %d instructions kept as data" %
              (sec, len(blob), ncode, len(raw)))


def symbolic_diag(src):
    """Makes the values that depend on the driver symbols, set by build.py."""
    def sub(old, new):
        nonlocal src
        if src.count(old) != 1:
            fail("DiagArea source: %r not found once" % old)
        src = src.replace(old, new)
    sub("\tmove.l #9184,d0\n", "\tmove.l #DRIVER_COPY_LEN,d0\n")
    tail = src[src.index("L00b0:\n"):]
    want = ("L00b0:\n\t.byte 0x01,0x23,0x45,0x67\n"
            "L00b4:\n\t.byte 0x4a,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xde,0x81,0x02,0x03,0x01\n"
            "L00c2:\n\t.byte 0x00,0x00,0x03,0xd8,0x00,0x00,0x04,0x23,0x00,0x00,0x00,0xce,0x00,0x00,0x03,0xac\n"
            "\t.byte 0x00,0x00,0x00,0x14,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x40,0x00,0x00\n")
    if tail != want:
        fail("DiagArea RomTag is not the one this was written for")
    return src.replace(tail, """L00b0:
\t.long 0x01234567\t\t| driver's address in RAM, set by DiagPoint
L00b4:\t\t\t\t\t| struct Resident, fixed up by DiagPoint
\t.short 0x4afc\t\t\t| RTC_MATCHWORD
\t.long 0\t\t\t\t| rt_MatchTag
\t.long 0xde\t\t\t| rt_EndSkip
\t.byte 0x81,2,3,1\t\t| RTF_COLDSTART|RTF_AUTOINIT, version 2, NT_DEVICE, priority 1
L00c2:
\t.long DRV_NAME\t\t\t| rt_Name, driver offset
\t.long DRV_IDSTRING\t\t| rt_IdString, driver offset
\t.long InitTable\t\t\t| rt_Init, DiagArea offset
InitTable:
\t.long 0x3ac\t\t\t| device data size
\t.long DRV_FUNCTABLE\t\t| driver offsets from here on
\t.long DRV_DATATABLE
\t.long DRV_INIT
\t.short 0
""")

if __name__ == "__main__":
    main()

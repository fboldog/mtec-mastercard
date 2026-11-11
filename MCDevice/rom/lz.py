"""The boot ROM's compression: a small LZ format, made for a short decoder
(diag.s) that reads the byte-wide ROM directly.

The stream is bytes and bit groups, interleaved in the order the decoder
reads them: when it needs a bit and has none left, the next stream byte is a
new group of 8, used from bit 7 down. Numbers are interlaced Elias gamma:
v = 1, then while the next bit is 0, v = v * 2 + the bit after it.

Tokens, the first bit chosen by what came before (as in zx0):

    after a copy, or at the start:
        0, gamma n               n literal bytes follow
        1, new copy
    after literal bytes:
        0, gamma n               copy n bytes from the last offset again
        1, new copy
    new copy: gamma h, byte l    offset (h - 1) * 256 + l + 1 back,
              gamma n            n + 1 bytes

The last offset starts at 1. The decoder stops when the output is full;
the size is in the ROM header.
"""


def gamma_bits(v):
    """Number of bits gamma(v) takes."""
    return 2 * v.bit_length() - 1


class _Writer:
    def __init__(self):
        self.out = bytearray()
        self.bitpos = None          # index of the current bit group
        self.nbits = 8

    def bit(self, b):
        if self.nbits == 8:
            self.bitpos = len(self.out)
            self.out.append(0)
            self.nbits = 0
        if b:
            self.out[self.bitpos] |= 0x80 >> self.nbits
        self.nbits += 1

    def gamma(self, v):
        assert v >= 1
        n = v.bit_length()
        for k in range(n - 2, -1, -1):
            self.bit(0)
            self.bit(v >> k & 1)
        self.bit(1)

    def byte(self, b):
        self.out.append(b)


def _matches(data, chain):
    """Per position: (offset, longest length), nearest first, each only
    where it is longer than every nearer one."""
    n = len(data)
    heads = {}
    cands = [()] * n
    for i in range(n - 1):
        prev = heads.setdefault(data[i:i + 2], [])
        lst = []
        best = 1
        for p in reversed(prev[-chain:]):
            ln = 0
            while i + ln < n and data[p + ln] == data[i + ln]:
                ln += 1
            if ln > best:
                lst.append((i - p, ln))
                best = ln
        prev.append(i)
        cands[i] = lst
    return cands


def compress(data, max_lit=255, chain=512):
    """Forward parse over two states (after a copy, after literals), each
    position keeping the cheapest way in and the last offset that brings."""
    data = bytes(data)
    n = len(data)
    cands = _matches(data, chain)
    INF = 1 << 60
    COPY, LIT = 0, 1
    cost = [[INF] * (n + 1), [INF] * (n + 1)]
    last = [[1] * (n + 1), [1] * (n + 1)]
    step = [[None] * (n + 1), [None] * (n + 1)]
    cost[COPY][0] = 0

    def relax(i, s, c, j, t, how, off):
        if c < cost[t][j]:
            cost[t][j] = c
            last[t][j] = off
            step[t][j] = (i, s, how)

    for i in range(n):
        for s in (COPY, LIT):
            c0 = cost[s][i]
            if c0 >= INF:
                continue
            lo = last[s][i]
            if s == COPY:
                for k in range(1, min(max_lit, n - i) + 1):
                    relax(i, s, c0 + 1 + gamma_bits(k) + 8 * k, i + k, LIT, ("L", k), lo)
            elif i >= lo:
                m = 0
                while i + m < n and data[i - lo + m] == data[i + m]:
                    m += 1
                for k in range(1, m + 1):
                    relax(i, s, c0 + 1 + gamma_bits(k), i + k, COPY, ("R", k), lo)
            nxt = 2
            for off, ln in cands[i]:
                oc = c0 + 1 + gamma_bits(((off - 1) >> 8) + 1) + 8
                lens = range(nxt, ln + 1) if ln - nxt < 64 else list(range(nxt, nxt + 64)) + [ln]
                for k in lens:
                    relax(i, s, oc + gamma_bits(k - 1), i + k, COPY, ("M", off, k), off)
                nxt = ln + 1
    # back from the cheaper end state
    s = COPY if cost[COPY][n] <= cost[LIT][n] else LIT
    steps, j = [], n
    while j:
        i, ps, how = step[s][j]
        steps.append(how)
        j, s = i, ps
    steps.reverse()
    w = _Writer()
    i = 0
    for how in steps:
        if how[0] == "L":
            w.bit(0)
            w.gamma(how[1])
            for b in data[i:i + how[1]]:
                w.byte(b)
            i += how[1]
        elif how[0] == "R":
            w.bit(0)
            w.gamma(how[1])
            i += how[1]
        else:
            _, off, k = how
            o = off - 1
            w.bit(1)
            w.gamma((o >> 8) + 1)
            w.byte(o & 0xFF)
            w.gamma(k - 1)
            i += k
    return bytes(w.out)


def decompress(src, size):
    """The decoder, as diag.s does it, for checking."""
    out = bytearray()
    pos = 0
    state = {"buf": 0x80}

    def getbit():
        nonlocal pos
        b = state["buf"] << 1
        if b & 0xFF == 0:
            b = src[pos] << 1 | (b >> 8)
            pos += 1
        state["buf"] = b & 0xFF
        return b >> 8 & 1

    def gamma():
        v = 1
        while not getbit():
            v = v * 2 + getbit()
        return v

    def copy(off, k):
        p = len(out) - off
        for _ in range(k):
            out.append(out[p])
            p += 1

    last = 1
    while len(out) < size:
        if not getbit():
            k = gamma()                         # literals
            out += src[pos:pos + k]
            pos += k
            if len(out) >= size:
                break
            if not getbit():
                copy(last, gamma())             # the last offset again
                continue
        h = gamma()                             # a new copy
        last = ((h - 1) << 8 | src[pos]) + 1
        pos += 1
        copy(last, gamma() + 1)
    return bytes(out)

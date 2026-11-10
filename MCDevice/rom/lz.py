"""The boot ROM's compression: a small LZ format, made for a short decoder
(diag.s) that reads the byte-wide ROM directly.

The stream is bytes and bit groups, interleaved in the order the decoder
reads them: when it needs a bit and has none left, the next stream byte is a
new group of 8, used from bit 7 down. Numbers are interlaced Elias gamma:
v = 1, then while the next bit is 0, v = v * 2 + the bit after it.

    0, gamma n               n literal bytes follow
    1, gamma h, byte l       copy from offset (h - 1) * 256 + l + 1 back,
       gamma n               n + 1 bytes

The decoder stops when the output is full; the size is in the ROM header.
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


def compress(data, max_lit=255, chain=512):
    data = bytes(data)
    n = len(data)
    # Match candidates per position: (offset, longest length), nearest first,
    # each one only where it is longer than every nearer one.
    heads = {}
    cands = [None] * n
    for i in range(n):
        lst = []
        if i + 1 < n:
            key = data[i:i + 2]
            prev = heads.get(key, [])
            best = 1
            for p in reversed(prev[-chain:]):
                ln = 0
                while i + ln < n and data[p + ln] == data[i + ln]:
                    ln += 1
                if ln > best:
                    lst.append((i - p, ln))
                    best = ln
            prev.append(i)
            heads[key] = prev
        cands[i] = lst
    # Cheapest encoding of data[i:], back to front.
    INF = 1 << 60
    cost = [INF] * (n + 1)
    step = [None] * (n + 1)
    cost[n] = 0
    for i in range(n - 1, -1, -1):
        best, how = INF, None
        for k in range(1, min(max_lit, n - i) + 1):
            c = 1 + gamma_bits(k) + 8 * k + cost[i + k]
            if c < best:
                best, how = c, ("L", k)
        lo = 2
        for off, ln in cands[i]:
            o = off - 1
            oc = 1 + gamma_bits((o >> 8) + 1) + 8
            lens = range(lo, ln + 1) if ln - lo < 64 else list(range(lo, lo + 64)) + [ln]
            for m in lens:
                c = oc + gamma_bits(m - 1) + cost[i + m]
                if c < best:
                    best, how = c, ("M", off, m)
            lo = ln + 1
        cost[i], step[i] = best, how
    w = _Writer()
    i = 0
    while i < n:
        s = step[i]
        if s[0] == "L":
            w.bit(0)
            w.gamma(s[1])
            for b in data[i:i + s[1]]:
                w.byte(b)
            i += s[1]
        else:
            _, off, m = s
            o = off - 1
            w.bit(1)
            w.gamma((o >> 8) + 1)
            w.byte(o & 0xFF)
            w.gamma(m - 1)
            i += m
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

    while len(out) < size:
        if not getbit():
            k = gamma()
            out += src[pos:pos + k]
            pos += k
        else:
            h = gamma()
            o = (h - 1) << 8 | src[pos]
            pos += 1
            p = len(out) - o - 1
            for _ in range(gamma() + 1):
                out.append(out[p])
                p += 1
    return bytes(out)

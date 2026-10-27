"""LZRW3-A decompressor matching DE_CLZRWContentsCompressor::Decompress (Core 0x7000c200)."""
START = b"123456789012345678"


def _hash(b, i):
    return (((((b[i] << 4) ^ b[i + 1]) << 4) ^ b[i + 2]) * 40543 >> 4) & 0x1FF


def decompress(src, out_len):
    if src[0] == 1:  # FLAG_COPY
        return bytes(src[4:])
    # Output buffer is prefixed with START so table pointers can be plain offsets.
    out = bytearray(START)
    base = len(START)
    table = [0] * 4096  # offsets into out; 0 == START
    cycle = 0
    pending = 0
    p, end = 4, len(src)
    ctrl = 1
    while p != end:
        if ctrl == 1:
            ctrl = src[p] | (src[p + 1] << 8) | 0x10000
            p += 2
        for _ in range(1 if p > end - 0x20 else 16):
            if p == end:
                break
            if ctrl & 1 == 0:
                out.append(src[p]); p += 1
                pending += 1
                if pending == 3:
                    q = len(out) - 3
                    table[cycle + _hash(out, q) * 8] = q
                    cycle = (cycle + 1) & 7
                    pending = 2
            else:
                b1, b2 = src[p], src[p + 1]; p += 2
                idx = ((b1 & 0xF0) << 4) | b2
                ref = table[idx]
                here = len(out)
                for k in range((b1 & 0xF) + 3):
                    out.append(out[ref + k])
                if pending:
                    q = here - pending
                    table[cycle + _hash(out, q) * 8] = q
                    cycle = (cycle + 1) & 7
                    if pending == 2:
                        table[cycle + _hash(out, q + 1) * 8] = q + 1
                        cycle = (cycle + 1) & 7
                    pending = 0
                table[(idx & ~7) + cycle] = here
                cycle = (cycle + 1) & 7
            ctrl >>= 1
    res = bytes(out[base:])
    assert len(res) == out_len, (len(res), out_len)
    return res

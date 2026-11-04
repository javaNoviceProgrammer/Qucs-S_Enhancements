#!/usr/bin/env python3
"""compare.py A.solution B.solution: two kept RF 2.5D solutions (rf25_kept.cpp's
format: "QRF25SOL", quint32 format, the stamps, a qCompress'd body) read
head and body; prints each head field that differs and every byte range
of the two bodies that differs - z, s, the currents and the rest are
written in member order, so equal answers leave only the stamps and the
threads (FullWave::threads) apart."""
import struct, sys, zlib


def head(raw):
    assert raw[:8] == b'QRF25SOL', 'no solution'
    o = 8
    fmt, = struct.unpack('>I', raw[o:o + 4]); o += 4
    out = {'format': fmt}
    for name in ('modelHash', 'layoutHash'):
        n, = struct.unpack('>I', raw[o:o + 4]); o += 4
        if n == 0xFFFFFFFF:
            out[name] = None
        else:
            out[name] = raw[o:o + n].hex(); o += n
    out['cells'], = struct.unpack('>i', raw[o:o + 4]); o += 4
    n, = struct.unpack('>I', raw[o:o + 4]); o += 4
    if n == 0xFFFFFFFF:
        out['solvedBy'] = None
    else:
        out['solvedBy'] = raw[o:o + n].decode('utf-16-be'); o += n
    # QDateTime (Qt_6_0): QDate as qint64, QTime as quint32, then the spec.
    jd, ms, spec = struct.unpack('>qIb', raw[o:o + 13]); o += 13
    out['solvedAt'] = (jd, ms, spec)
    out['kind'] = raw[o]; o += 1
    n, = struct.unpack('>I', raw[o:o + 4]); o += 4
    packed = raw[o:o + n]
    size, = struct.unpack('>I', packed[:4])
    body = zlib.decompress(packed[4:])
    assert len(body) == size, (len(body), size)
    return out, body


def ranges(a, b):
    out, start = [], None
    for i in range(max(len(a), len(b))):
        same = i < len(a) and i < len(b) and a[i] == b[i]
        if not same and start is None:
            start = i
        if same and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, max(len(a), len(b))))
    return out


def main():
    ha, ba = head(open(sys.argv[1], 'rb').read())
    hb, bb = head(open(sys.argv[2], 'rb').read())
    for k in ha:
        if ha[k] != hb[k]:
            print('head %s: %r / %r' % (k, ha[k], hb[k]))
    print('bodies %d and %d bytes' % (len(ba), len(bb)))
    rs = ranges(ba, bb)
    print('%d range(s) apart' % len(rs))
    for s, e in rs[:20]:
        print('  [%d, %d): %s / %s' % (s, e, ba[s:e].hex(), bb[s:e].hex()))
    return 0


if __name__ == '__main__':
    sys.exit(main())

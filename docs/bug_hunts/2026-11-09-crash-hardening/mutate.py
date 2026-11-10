# Mutations of a file's bytes for the hunt of 9 November 2026: blind ones
# (flips, truncation, splices, repeats) and ones that know text (a number
# made huge, negative, NaN or empty; a line dropped, repeated or swapped; a
# token's quotes or brackets broken; an XML element unclosed or nested deep).
# mutate(data, rnd) returns new bytes; a few mutations are stacked.
import re

HOSTILE_NUMBERS = [b'0', b'-0', b'-1', b'1e308', b'-1e308', b'1e-320', b'nan', b'NaN', b'inf', b'-inf',
                   b'2147483647', b'2147483648', b'-2147483648', b'4294967296', b'9223372036854775807',
                   b'18446744073709551616', b'99999999999999999999', b'0x7fffffff', b'', b'1e', b'.', b'--1',
                   b'1.#INF', b'3.4e38', b'65536', b'1000000000', b'-999999']
HOSTILE_TOKENS = [b'', b'"', b"'", b'<', b'>', b'{', b'}', b'[', b']', b'(', b')', b'\x00', b'\xff\xfe', b'\xc3',
                  b'\r', b'\n', b'\t', b'=', b';', b',', b'#', b'%', b'\\', b'&amp', b'<!--', b']]>', b'<?xml',
                  b'A' * 70000, b'\xe2\x80\xae', b'..', b'/', b'*']
NUM = re.compile(rb'-?\d+(\.\d+)?([eE][-+]?\d+)?')


def _flip(d, r):
    if not d: return d
    d = bytearray(d)
    for _ in range(r.randint(1, 8)):
        i = r.randrange(len(d)); d[i] ^= 1 << r.randrange(8)
    return bytes(d)


def _byte(d, r):
    if not d: return d
    d = bytearray(d)
    for _ in range(r.randint(1, 4)):
        d[r.randrange(len(d))] = r.choice([0, 0xff, 0x7f, 0x80, ord('"'), ord('<'), ord('\n'), r.randrange(256)])
    return bytes(d)


def _truncate(d, r):
    return d[:r.randrange(len(d) + 1)] if d else d


def _cut(d, r):
    if len(d) < 2: return d
    a = r.randrange(len(d)); b = min(len(d), a + r.choice([1, 4, 16, 256, 4096]))
    return d[:a] + d[b:]


def _splice(d, r):
    if len(d) < 2: return d
    a = r.randrange(len(d)); b = min(len(d), a + r.choice([1, 8, 64, 1024]))
    at = r.randrange(len(d))
    return d[:at] + d[a:b] * r.choice([1, 2, 16, 200]) + d[at:]


def _number(d, r):
    ms = list(NUM.finditer(d))
    if not ms: return _byte(d, r)
    for _ in range(r.randint(1, 3)):
        m = r.choice(ms)
        d = d[:m.start()] + r.choice(HOSTILE_NUMBERS) + d[m.end():]
        ms = list(NUM.finditer(d))
        if not ms: break
    return d


def _token(d, r):
    at = r.randrange(len(d) + 1)
    return d[:at] + r.choice(HOSTILE_TOKENS) + d[at:]


def _lines(d, r):
    ls = d.split(b'\n')
    if len(ls) < 2: return _token(d, r)
    op = r.randrange(5)
    i = r.randrange(len(ls)); j = r.randrange(len(ls))
    if op == 0: del ls[i]
    elif op == 1: ls.insert(j, ls[i])
    elif op == 2: ls[i], ls[j] = ls[j], ls[i]
    elif op == 3: ls[i:i] = [ls[i]] * r.choice([10, 1000])
    else:
        a, b = sorted((i, j)); del ls[a:b + 1]
    return b'\n'.join(ls)


def _xml(d, r):
    tags = list(re.finditer(rb'<(/?)([A-Za-z_][\w:.-]*)', d))
    if not tags: return _lines(d, r)
    m = r.choice(tags)
    op = r.randrange(5)
    if op == 0:   # an element's tag dropped
        e = d.find(b'>', m.start())
        return d[:m.start()] + d[e + 1 if e >= 0 else m.end():]
    if op == 1:   # renamed
        return d[:m.start(2)] + r.choice([b'x', m.group(2) + b'x', b'', b'1']) + d[m.end(2):]
    if op == 2:   # nested deep
        name = m.group(2)
        return d[:m.start()] + (b'<' + name + b'>') * r.choice([100, 5000]) + d[m.start():]
    if op == 3:   # an attribute's value
        a = list(re.finditer(rb'="([^"]*)"', d))
        if a:
            x = r.choice(a)
            v = r.choice(HOSTILE_NUMBERS + [b'', b'&#0;', b'&undefined;', b'A' * 5000, b'-1 -1 -1', b'1,2,3,,'])
            return d[:x.start(1)] + v + d[x.end(1):]
        return d
    # an element repeated
    e = d.find(b'>', m.start())
    if e < 0: return d
    return d[:m.start()] + d[m.start():e + 1] * r.choice([2, 50]) + d[m.start():]


OPS = [_flip, _byte, _truncate, _cut, _splice, _number, _number, _token, _lines, _lines, _xml]


def mutate(data, r, xml=False, n=None):
    ops = OPS + ([_xml, _xml] if xml else [])
    for _ in range(n or r.choice([1, 1, 1, 2, 3])):
        data = r.choice(ops)(data, r)
    return data

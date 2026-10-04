"""p14: a binary dataset damaged in its index and footer (text) and values, read by get_dataset and a diagram, on the
ASan build: never a crash or a sanitizer report."""
import os, random, re, shutil, mcp
mcp.APP = '<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
s = mcp.Server('p14')
s.call('set_settings', {'scope': 'simulators', 'values': {'Results/Keep large results binary': True, 'Results/Binary above': 0}})
s.call('import_netlist', {'text': 'rc\nV1 in 0 DC 1 AC 1 PULSE(0 1 0 1n 1n 5u 10u)\nR1 in out 1k\nC1 out 0 1n\n.ac dec 10 1k 10meg\n.tran 100n 20u\n.end',
                          'save_as': s.ws + '/rc.sch'})
s.call('add_analysis', {'path': 'rc.sch', 'kind': 'sweep', 'analysis': 'AC1', 'parameter': 'C1', 'from': '1n', 'to': '4n', 'points': 3})
s.call('add_diagram', {'path': 'rc.sch', 'traces': [{'variable': 'ac.v(out)'}, {'variable': 'tran.v(out)'}]})
sim = s.call('simulate', {'path': 'rc.sch'})
good = open(s.ws + '/rc.dat.ngspice', 'rb').read()
assert good.startswith(b'<Qucs Dataset') and b'binary' in good[:64], good[:64]
at = good.rfind(b'<index>')
rng = random.Random(1004)
def mutate(b):
    b = bytearray(b)
    k = rng.randrange(9)
    if k == 0:   # a number in the index changed
        idx = b[at:]
        nums = [m for m in re.finditer(rb'\d+', bytes(idx))]
        m = rng.choice(nums); v = rng.choice([b'0', b'7', b'9' * 19, b'-8', str(int(m.group()) * 2).encode(), str(int(m.group()) + 8).encode()])
        return bytes(b[:at + m.start()] + v + b[at + m.end():])
    if k == 1:   # a header word in the index changed
        idx = bytes(b[at:]); lines = idx.split(b'\n')
        i = rng.randrange(1, max(2, len(lines) - 3)); w = lines[i].split(b' ')
        if len(w) > 1: w[rng.randrange(len(w))] = rng.choice([b'', b'<dep', b'x', b'dep', b'indep', b'>', b'r', b'c', b'time'])
        lines[i] = b' '.join(w); return bytes(b[:at]) + b'\n'.join(lines)
    if k == 2:   # an index line duplicated or dropped
        idx = bytes(b[at:]); lines = idx.split(b'\n'); i = rng.randrange(1, max(2, len(lines) - 3))
        if rng.random() < .5: lines.insert(i, lines[i])
        else: del lines[i]
        return bytes(b[:at]) + b'\n'.join(lines)
    if k == 3:   # the footer's numbers
        f = bytearray(b[-43:]); p = rng.choice(list(range(9, 25)) + list(range(26, 42))); f[p] = ord(rng.choice('0123456789abcdefFz-'))
        return bytes(b[:-43] + f)
    if k == 4:   # values: random bytes (NaN, Inf)
        for _ in range(rng.randrange(1, 50)):
            p = rng.randrange(64, at - 8); b[p:p + 8] = rng.choice([b'\x00\x00\x00\x00\x00\x00\xf8\x7f', b'\x00\x00\x00\x00\x00\x00\xf0\x7f', os.urandom(8)])
        return bytes(b)
    if k == 5: return bytes(b[:rng.randrange(0, len(b))])                       # truncated
    if k == 6: return bytes(b[:64] + b[64 + 8 * rng.randrange(1, 100):])        # values shifted
    if k == 7:   # the first line
        b[rng.randrange(0, 40)] = rng.randrange(256); return bytes(b)
    return bytes(b[:at]) + bytes(b[at:]).replace(b' c\n', b' r\n', 1) if rng.random() < .5 else bytes(b[:at]) + bytes(b[at:]).replace(b' r\n', b' c\n', 1)
dead = None
for i in range(120):
    m = mutate(good)
    open(s.ws + '/rc.dat.ngspice', 'wb').write(m)
    try:
        s.call('reload_data', {'path': 'rc.sch'})
        s.call('get_dataset', {'path': 'rc.sch'})
        for v in ('ac.v(out)', 'tran.v(out)', 'ac.v(in)'):
            s.call('get_dataset', {'path': 'rc.sch', 'variables': [v], 'points': 3, 'measure': ['max', 'bandwidth']})
        s.call('screenshot', {'path': 'rc.sch', 'max_size': 200}) if i % 10 == 0 else None
    except Exception as e:
        dead = (i, repr(e)[:500]); open(s.root + '/killer.dat', 'wb').write(m); break
err = open(s.root + '/server.err', errors='replace').read()
print('mutants run:', i + 1, '| died:', dead)
print('sanitizer reports:', len(re.findall(r'ERROR: AddressSanitizer|runtime error:', err)))
for l in re.findall(r'.*(?:ERROR: AddressSanitizer|runtime error:).*', err)[:5]: print('  ', l[:300])
s.close()

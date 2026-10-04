"""p27: check_schematic of 100 distinct installed library parts, with no search paths and with 3000 (empty) ones."""
import os, re, time, mcp
lib = '<repo>/qucs-s-26.1.1/library'
parts = []
for f in sorted(os.listdir(lib)):
    if not f.endswith('.lib'): continue
    for c in re.findall(r'\n<Component ([^>]+)>', open(os.path.join(lib, f), errors='replace').read()):
        if ' ' not in c and '"' not in c: parts.append((f[:-4], c))
parts = parts[::max(1, len(parts) // 100)][:100]
body = ''.join(f'  <Lib X{i} 1 {100 + (i % 10) * 150} {100 + (i // 10) * 150} 20 -20 0 0 "{l}" 0 "{c}" 0>\n' for i, (l, c) in enumerate(parts))
for n in (0, 3000):
    s = mcp.Server(f'p27-{n}')
    paths = []
    for i in range(n):
        d = f'{s.root}/v/d{i:04d}'; os.makedirs(d); paths.append(d)
    if paths: s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': paths}})
    open(s.ws + '/many.sch', 'w').write('<Qucs Schematic 26.1.5>\n<Components>\n' + body + '</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
    t = time.time(); s.call('open_document', {'path': s.ws + '/many.sch'}); to = time.time() - t
    times = []
    for _ in range(3):
        t = time.time(); c = s.call('check_schematic', {'path': 'many.sch'}); times.append(time.time() - t)
    print(f'{n} search paths: open {to:.2f} s, check {min(times):.2f} s (best of 3), {len(parts)} distinct parts')
    s.close()

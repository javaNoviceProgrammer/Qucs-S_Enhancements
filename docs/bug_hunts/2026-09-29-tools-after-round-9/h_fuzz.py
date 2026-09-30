import json, os, sys, glob, random, re, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; seed = int(sys.argv[2]); budget = float(sys.argv[3])
random.seed(seed)
WS = HERE + '/wsfz'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
files = sorted(glob.glob(HERE + '/exs/**/*.sch', recursive=True))
weird = ['', '-1', '0', '1e308', '-1e308', 'nan', 'inf', '1/0', '{x}', 'abc', '1kk', '  ', '9' * 400, '-0', '1e-320', '"', '\\', '1 V', '-15 V', '0.0001p']
def mutate(text):
    lines = text.split('\n')
    out = []
    in_comp = in_wire = False
    for l in lines:
        s = l.strip()
        if s == '<Components>': in_comp = True
        elif s == '</Components>': in_comp = False
        elif s == '<Wires>': in_wire = True
        elif s == '</Wires>': in_wire = False
        elif in_comp and s.startswith('<') and random.random() < 0.5:
            vals = list(re.finditer(r'"([^"]*)"', l))
            if vals:
                m = random.choice(vals)
                w = random.choice(weird).replace('"', "'")
                l = l[:m.start(1)] + w + l[m.end(1):]
        elif in_wire and s.startswith('<'):
            if random.random() < 0.08: continue
            if random.random() < 0.1:
                l = re.sub(r'"([^"]*)" (\-?\d+) (\-?\d+) (\-?\d+) "', lambda m: '"%s" %s %s %s "' % (random.choice(['gnd', '0', 'GND', 'out', 'Out', 'VEE', 'vcc', 'x.y', 'a b']), m.group(2), m.group(3), m.group(4)), l)
        out.append(l)
    return '\n'.join(out)
s = Server(WS)
t0 = time.time(); n = 0; errs = []
while time.time() - t0 < budget:
    f = random.choice(files)
    try: text = open(f, errors='replace').read()
    except Exception: continue
    name = 'fz%d.sch' % n; n += 1
    open(WS + '/' + name, 'w').write(mutate(text))
    try:
        s.call('open_document', {'path': WS + '/' + name}, ok=False)
        s.call('check_schematic', {'path': name, 'subcircuits': True}, ok=False)
        s.call('get_netlist', {'path': name, 'map': True}, ok=False)
        s.call('arrange', {'path': name, 'preview': True, 'feedback': random.choice(['below', 'above']), 'straighten': True}, ok=False)
        s.call('close_document', {'path': name, 'unsaved': 'discard'}, ok=False)
    except Exception as e:
        errs.append((name, os.path.relpath(f, HERE + '/exs'), repr(e)[:120]))
        try: s.close()
        except Exception: pass
        s = Server(WS)
try: s.close()
except Exception: pass
print(n, 'files', len(errs), 'server deaths')
for e in errs[:10]: print(e)

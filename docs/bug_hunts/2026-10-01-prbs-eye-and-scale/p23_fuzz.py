import sys, json, os, random, time, shutil
sys.path.insert(0, '/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/ff1f864c-ed6a-4c31-a788-5998e059b547/scratchpad/repro')
from client import Server
root, seed, budget = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
os.makedirs(root + '/ws/p', exist_ok=True)
os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
random.seed(seed)
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
shutil.copy('/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/ff1f864c-ed6a-4c31-a788-5998e059b547/scratchpad/hunt/p4/ws/p/e.dat.ngspice', root + '/ws/p/f.dat.ngspice')
comps = ''.join('  <R R%d 1 %d 100 15 -26 0 1 "1k" 1>\n  <GND * 1 %d 160 0 0 0 0>\n' % (k, 100*k, 100*k) for k in range(1, 6))
comps += '  <vPRBS V1 1 700 300 18 -26 0 1 "0 V" 1 "1 V" 1 "50 ps" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0>\n  <GND * 1 700 330 0 0 0 0>\n'
text = ('<Qucs Schematic 26.1.5>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n' + comps +
        '</Components>\n<Wires>\n  <700 270 700 240 "out" 710 220 0 "">\n</Wires>\n<Diagrams>\n'
        '  <Eye 100 720 420 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 1 0 0 0 -1 - 2 0 2 - 0 1 0.3 0.2 "" "" "" "eye">\n'
        '\t<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>\n  </Eye>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(root + '/ws/p/f.sch', 'w').write(text)
s = Server(root, '/Users/meisam/git/Qucs-S_Enhancements', APP)
s.call('open_document', {'path': root + '/ws/p/f.sch'})
names = ['R1', 'R2', 'R5', 'V1', 'GND', 'GND#1', 'GND#3', 'GND#6', 'GND#0', 'gnd#2', 'X9', '', '#', 'GND#99999999999', 'R1#1']
vals = [None, True, False, 0, 1, -1, 2**31, -2**31, 1e308, float('nan') if False else 0.5, '', 'x', [], [1], {}, 'a' * 3000, 'é漢字', '1e-10', '100p', -5, 9]
def rnames(): return random.sample(names, random.randint(0, 5)) if random.random() < 0.8 else random.choice(vals)
gens = {
 'select': lambda: {'names': rnames()},
 'move': lambda: {random.choice(['names', 'selection']): rnames() if random.random() < .5 else True, 'dx': random.choice([0, 10, -10, 1e9, 'x']), 'dy': random.choice([0, 20, -20]), 'preview': random.random() < .5},
 'delete': lambda: {'names': rnames(), 'preview': random.random() < .6},
 'undo': lambda: random.choice([{}, {'to': random.choice(vals)}]),
 'redo': lambda: random.choice([{}, {'to': random.choice(vals)}]),
 'undo_history': lambda: {'steps': random.choice(vals)},
 'diff': lambda: random.choice([{}, {'steps': random.choice(vals)}]),
 'edit_diagram': lambda: {'diagram': random.choice([1, 2, 0, 'x']), 'eye': {random.choice(['unit_interval', 'span', 'from', 'levels', 'threshold', 'drawn', 'measurements', 'mask']): random.choice(vals + [{'width': 0.5, 'height': 0.1}, 'traces', 'density'])}},
 'get_dataset': lambda: {'variables': random.choice([['tran.v(out)'], ['out'], [], ['x']]), 'measure': ['eye'], random.choice(['bit_period', 'levels', 'offset', 'level']): random.choice(vals)},
 'edit_component': lambda: {'name': random.choice(names), 'properties': {random.choice(['R', 'Tbit', 'Order', 'Coding', 'Seed']): random.choice(['1k', '0', '-1', '', 'abc', '1e400', 'PAM4'])}, 'preview': random.random() < .3},
 'add_component': lambda: {'type': random.choice(['GND', 'R', 'vPRBS', 'MUTX', 'EDD']), 'x': random.randrange(-500, 2000), 'y': random.randrange(-500, 2000), 'preview': random.random() < .3},
 'check_schematic': lambda: {},
 'screenshot': lambda: {},
 'get_state': lambda: {},
}
t0 = time.time(); n = 0; errs = 0
while time.time() - t0 < budget:
    tool = random.choice(list(gens)); args = gens[tool]()
    try:
        e, t = s.call(tool, args); n += 1; errs += e
    except Exception as ex:
        print('SERVER DIED after %d calls at %s %s: %s' % (n, tool, json.dumps(args)[:300], ex)); break
s.close() if s.p.poll() is None else None
err = open(root + '/server.err').read()
print('seed %d: %d calls (%d refused), sanitizer reports %d, server %s' % (seed, n, errs, err.count('Sanitizer') + err.count('runtime error:'), 'alive' if n else '?'))
print('\n'.join([l for l in err.splitlines() if 'Sanitizer' in l or 'runtime error' in l or '#0 ' in l or '#1 ' in l][:12]))

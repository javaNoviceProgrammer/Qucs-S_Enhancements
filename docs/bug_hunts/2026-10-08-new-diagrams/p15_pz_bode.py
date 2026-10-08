"""An RLC low-pass (R 100, L 1m, C 1u: f0 5.03 kHz, Q 0.316) with .AC and .PZ: the pole-zero map, Bode, Nichols on real runs; numbers checked by hand."""
import cmath, json, math, sys, mcp
from common import show
s = mcp.Server('p15')
def c(label, tool, args, n=500):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
s.call('new_project', {'name': 'pz'}); s.call('open_project', {'name': 'pz'})
s.call('new_document', {})
s.call('add_component', {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'R', 'name': 'R1', 'x': 200, 'y': 120, 'properties': {'R': '100'}})
s.call('add_component', {'type': 'L', 'name': 'L1', 'x': 300, 'y': 120, 'properties': {'L': '1m'}})
s.call('add_component', {'type': 'C', 'name': 'C1', 'x': 400, 'y': 200, 'properties': {'C': '1u'}})
for a, b in (('V1.1', 'R1.1'), ('R1.2', 'L1.1'), ('L1.2', 'C1.1'), ('V1.2', 'ground'), ('C1.2', 'ground')): s.call('connect', {'from': a, 'to': b})
s.call('set_label', {'at': 'V1.1', 'name': 'in'}); s.call('set_label', {'at': 'C1.1', 'name': 'out'})
c('ac', 'add_analysis', {'kind': 'ac', 'from': '100 Hz', 'to': '1 MHz', 'points': 201}, 150)
for t in ('.PZ', 'PZ', 'SP_PZ'):
    r = c(f'add {t}', 'add_component', {'type': t, 'name': 'PZ1', 'x': 100, 'y': 400, 'properties': {'Input': 'in 0', 'Output': 'out 0'}}, 200)
    if not s.last_error: break
c('save', 'save_document', {'as': 'rlc.sch'}, 120)
sim = c('simulate', 'simulate', {'simulator': 'ngspice'}, 300)
print('vars', [v['name'] for v in s.call('get_dataset', {'points': 0}).get('variables', [])])
# by hand: poles of 1/(LC s^2 + RC s + 1)
L, C, R = 1e-3, 1e-6, 100
disc = cmath.sqrt((R / L) ** 2 - 4 / (L * C))
print('poles by hand', [(-R / L + disc) / 2, (-R / L - disc) / 2])
r = c('pole_zero of pole', 'add_diagram', {'type': 'pole_zero', 'traces': ['pole']}, 900)
c('get_dataset roots', 'get_dataset', {'variables': ['pole'], 'measure': ['roots']}, 1200)
# loop gain T = 10 * v(out)/v(in) (an amplifier of 10 around the filter): crosses 0 dB, phase goes to -180 (2nd order: never past; GM infinite)
r = c('bode of 10*v(out)/v(in)', 'add_diagram', {'type': 'bode', 'traces': ['10*ac.v(out)/ac.v(in)']}, 1500)
c('get_dataset margins', 'get_dataset', {'variables': ['10*ac.v(out)/ac.v(in)'], 'measure': ['phase_margin', 'gain_margin']}, 1500)
# by hand: |T| = 1 where 10/|1 - w^2 LC + j w RC| = 1
best = None
for i in range(200001):
    f = 100 * 10 ** (i / 200000 * 4)
    w = 2 * math.pi * f
    T = 10 / complex(1 - w * w * L * C, w * R * C)
    if best is None or abs(abs(T) - 1) < abs(abs(best[1]) - 1): best = (f, T)
print('by hand: crossover', best[0], 'phase margin', 180 + math.degrees(cmath.phase(best[1])))
c('nichols', 'add_diagram', {'type': 'nichols', 'traces': ['10*ac.v(out)/ac.v(in)']}, 900)
c('export', 'export_image', {'save_as': s.root + '/all.png'}, 200)
print('alive', s.p.poll() is None, s.root)
s.close()

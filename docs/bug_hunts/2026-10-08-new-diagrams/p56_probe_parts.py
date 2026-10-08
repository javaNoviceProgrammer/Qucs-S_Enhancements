"""Probe every pin of a diode, an inductor, a DC voltage source, a DC current source and a MOSFET, one at a time: the vector each gives, and whether the run still succeeds (Release build)."""
import mcp
from common import show
def bench(s):
    s.call('new_project', {'name': 'pp'}); s.call('open_project', {'name': 'pp'}); s.call('new_document', {})
    add = lambda t, n, x, y, pr=None: s.call('add_component', {'type': t, 'name': n, 'x': x, 'y': y, 'properties': pr or {}})
    add('Vdc', 'V1', 100, 200, {'U': '5 V'}); add('R', 'R1', 200, 120, {'R': '1k'}); add('Diode', 'D1', 300, 200); add('L', 'L1', 400, 120, {'L': '1m'})
    add('R', 'R2', 500, 200, {'R': '1k'}); add('Idc', 'I1', 600, 200, {'I': '1 mA'}); add('R', 'R3', 700, 200, {'R': '1k'})
    add('_MOSFET', 'M1', 800, 200); add('R', 'R4', 800, 100, {'R': '1k'})
    for a, b in (('V1.1', 'R1.1'), ('R1.2', 'D1.1'), ('D1.2', 'ground'), ('V1.2', 'ground'), ('R1.2', 'L1.1'), ('L1.2', 'R2.1'), ('R2.2', 'ground'),
                 ('I1.1', 'R3.1'), ('I1.2', 'ground'), ('R3.2', 'ground'), ('M1.1', 'V1.1'), ('M1.2', 'R4.1'), ('R4.2', 'V1.1'), ('M1.3', 'ground')):
        r = s.call('connect', {'from': a, 'to': b})
        if s.last_error: print('connect', a, b, str(r)[:100])
    s.call('add_analysis', {'kind': 'tran', 'to': '1 ms'}); s.call('save_document', {'as': 'pp.sch'})
s = mcp.Server('p56'); bench(s)
comps = {c['name']: len(c['pins']) for c in s.call('get_schematic', {})['components'] if c['name'] in ('D1', 'L1', 'V1', 'I1', 'M1')}
base = s.call('simulate', {'simulator': 'ngspice'}); print('without a probe:', base.get('succeeded'), base.get('errors')); s.close()
print('pins', comps)
for name, n in comps.items():
    for pin in range(1, n + 1):
        s = mcp.Server('p56'); bench(s)
        d = s.call('add_diagram', {'type': 'rect'})['diagram']
        r = s.call('probe', {'what': f'{name}.{pin}', 'diagram': d})
        sim = s.call('simulate', {'simulator': 'ngspice'})
        v = r.get('variable') if isinstance(r, dict) else str(r)[:80]
        pts = None
        if sim.get('succeeded'):
            g = next(x for x in s.call('get_schematic', {})['diagrams'] if x['diagram'] == d)
            pts = [(t.get('points'), (t.get('no data') or '')[:60]) for t in g['traces']]
        print(f'{name}.{pin}: {v} -> run {sim.get("succeeded")} {sim.get("errors") or ""} {pts}', flush=True)
        s.close()

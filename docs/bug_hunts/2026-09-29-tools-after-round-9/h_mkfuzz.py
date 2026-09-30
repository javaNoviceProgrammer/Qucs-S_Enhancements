import json, os, sys, random, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
WS = HERE + '/wsmk'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
A = lambda **a: {'tool': 'add_component', 'arguments': a}
C = lambda a, b: {'tool': 'connect', 'arguments': {'from': a, 'to': b}}
s = Server(WS)
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [A(type='Vac', name='V1', x=100, y=200, properties={'U': '1 V'}), A(type='R', name='R1', x=250, y=100),
  A(type='C', name='C1', x=400, y=200, rotation=1, properties={'C': '100n'}), C('V1.1', 'R1.1'), C('R1.2', 'C1.2'), C('C1.1', 'ground'), C('V1.2', 'ground'),
  {'tool': 'set_label', 'arguments': {'at': 'R1.2', 'name': 'out'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'ac', 'from': '10 Hz', 'to': '1 MHz', 'points': 101}},
  {'tool': 'save_document', 'arguments': {'as': WS + '/mk.sch'}}]})
s.call('simulate', {'path': 'mk.sch', 'brief': True})
for t in ['rect', 'polar', 'smith', 'admittance_smith', 'polar_smith']:
    s.call('add_diagram', {'path': 'mk.sch', 'type': t, 'traces': ['ac.v(out)']}, ok=False)
nots = ['diagram', 'automatic', 'decimal', 'scientific', 'power_of_ten', 'engineering', 'engineering_exponent', 'bogus', 5, None]
t0 = time.time(); n = 0; deaths = 0
while time.time() - t0 < budget:
    d = random.randint(1, 6); n += 1
    args = {'path': 'mk.sch', 'diagram': d}
    for k, choices in [('notation', nots), ('precision', [0, 1, 3, 12, 13, -1, 'x']), ('format', ['real_imaginary', 'magnitude_degrees', 'magnitude_radians', 'x']),
                       ('at', [10, 1e6, 'peak', 'min', '-3dB', 'crossing:0.5', 1e308, -1])]:
        if random.random() < 0.6: args[k] = random.choice(choices)
    tool = random.choice(['add_marker', 'edit_marker', 'edit_marker', 'edit_diagram'])
    if tool == 'edit_diagram': args = {'path': 'mk.sch', 'diagram': d, 'notation': random.choice(nots[1:7]), 'decimals': random.choice([-1, 0, 3, 15])}
    try:
        s.call(tool, args, ok=False)
        if n % 25 == 0:
            s.call('get_schematic', {'path': 'mk.sch'}, ok=False); s.call('export_image', {'path': 'mk.sch', 'save_as': WS + '/x.png'}, ok=False)
            s.call('save_document', {'path': 'mk.sch'}, ok=False)
    except Exception as e:
        deaths += 1; print('died', tool, json.dumps(args)[:150], repr(e)[:80])
        try: s.close()
        except Exception: pass
        s = Server(WS); s.call('open_document', {'path': WS + '/mk.sch'}, ok=False)
g = s.call('get_schematic', {'path': 'mk.sch'}, ok=False)
print(n, 'calls,', deaths, 'deaths;', sum(len(dd.get('markers', [])) for dd in g.get('diagrams', [])) if isinstance(g, dict) else g[:100], 'markers now')
s.close()

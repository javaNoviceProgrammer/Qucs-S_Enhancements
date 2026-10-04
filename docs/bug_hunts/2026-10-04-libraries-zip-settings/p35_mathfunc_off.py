"""The ngspice_mathfunc.inc setting off, a B source using limexp/step: the netlist, the check, the run."""
import sys, json; sys.path.insert(0, __file__.rsplit('/', 1)[0]); from mcp import Server
s = Server('p35')
key = 'Netlist/Include ngspice_mathfunc.inc (limexp, step, stp)'
def build():
    s.call('new_document', {'kind': 'schematic'})
    s.call('add_component', {'type': 'src_eqndef', 'x': 100, 'y': 100, 'properties': {'V': 'limexp(time*1e3) + step(time-1m)'}})
    s.call('add_component', {'type': 'R', 'x': 200, 'y': 100, 'properties': {'R': '1k'}})   # pins 170,100 / 230,100
    s.call('add_component', {'type': 'GND', 'x': 100, 'y': 200})
    s.call('add_component', {'type': '.TR', 'x': 300, 'y': 300, 'properties': {'Stop': '2m'}})
    print(' wires', str(s.call('add_wire', {'points': [[100, 70], [170, 70], [170, 100]]}))[:100],
          str(s.call('add_wire', {'points': [[100, 130], [100, 200]]}))[:100],
          str(s.call('add_wire', {'points': [[230, 100], [230, 200], [100, 200]]}))[:100])
for on in (False, True):
    print('== include', on, json.dumps(s.call('set_settings', {'scope': 'simulators', 'values': {key: on}}).get('changed')))
    build()
    n = str(s.call('get_netlist', {})); print('netlist head:', [l for l in n.splitlines() if 'INCLUDE' in l or l.startswith('B1')])
    c = s.call('check_schematic', {}); print('check:', c.get('found'), [w['message'] for w in c.get('warnings', []) + c.get('errors', [])][:5])
    r = s.call('simulate', {'timeout': 60}); t = json.dumps(r)
    print('simulate:', t[:200], '...', [k for k in r] if isinstance(r, dict) else '', '| limexp in output:', 'limexp' in t, t[t.find('rror'):t.find('rror')+300] if 'rror' in t else '')
s.close()

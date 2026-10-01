# One net, two nodes: a wire with a label and a ground symbol at its end - from a file, and built by the tools.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_split'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
def nl(path=None):
    net = s.call('get_netlist', {'path': path} if path else {}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    return [l for l in net.splitlines() if l[:2] in ('V1', 'R1', 'R2')]
def chk(path=None):
    r = s.call('check_schematic', {'path': path} if path else {}, ok=False)
    return [i['message'][:90] for k in ('errors', 'warnings') for i in (r.get(k, []) if isinstance(r, dict) else [])]
HEAD = '<Qucs Schematic 26.1.4>\n<Components>\n'
V = '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n'
R = '  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
DC = '  <.DC DC1 1 200 40 0 40 0 0 "26.85" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "no" 0 "150" 0 "no" 0 "none" 0 "CroutLU" 0>\n'
f = ws + '/foo.sch'
open(f, 'w').write(HEAD + V + R + DC + '  <GND * 1 100 90 0 0 0 0>\n</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n'
                   '  <0 90 100 90 "foo" 50 110 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
s.call('open_document', {'path': f})
print('file, label foo, ground at the end:', nl(f), '| check:', chk(f))
r = s.call('simulate', {'path': f}, ok=False)
print('  simulate:', r.get('succeeded') if isinstance(r, dict) else r[:200], [e.get('message') for e in r.get('errors', [])][:2] if isinstance(r, dict) else '')
# By the tools: a net labelled, then a pin of it connected to ground.
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 300, 'y': 200}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 500, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}},
  {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'R1.2'}},
  {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}},
  {'tool': 'connect', 'arguments': {'from': 'R2.1', 'to': 'R1.1'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}]})
sch = s.call('get_schematic', {})
net_of = {p['net'] for c in sch['components'] if c['name'] == 'R1' for p in c.get('pins', []) if p.get('pin') in (2, '2') or True}
wires = sch.get('wires', [])
print('nets:', [ (n.get('name'), n.get('pins')) for n in sch.get('nets', [])][:4])

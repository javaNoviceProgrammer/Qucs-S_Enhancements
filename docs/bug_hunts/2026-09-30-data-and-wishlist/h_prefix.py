# make_symbol's prefix: what it takes, and the names of the instances placed after.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_prefix'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
s.call('new_document', {})
s.call('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R1.2'}},
  {'tool': 'save_document', 'arguments': {'as': 'sub.sch'}}]})
for prefix in ('A B', '', '1X', 'é', 'X;Y', 'R', 'V', 'GND'):
    r = s.call('make_symbol', {'path': 'sub.sch', 'prefix': prefix}, ok=False)
    s.call('save_document', {'path': 'sub.sch'}, ok=False)
    s.call('new_document', {})
    p = s.call('add_component', {'type': 'Sub', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}, ok=False)
    name = p.get('name') if isinstance(p, dict) else p[:120]
    net = s.call('get_netlist', {}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    print(repr(prefix), '| make_symbol:', (r if isinstance(r, str) else 'ok ' + str(r.get('prefix', '')))[:90], '| placed as', name, '|', [l for l in net.splitlines() if l.upper().startswith('X')][:1])
    s.call('close_document', {'unsaved': 'discard'}, ok=False)
s.close()

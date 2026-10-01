# The wishlist's sources (Vac/Iac with ACmag, Vpulse's period) as each SPICE simulator's netlist writes them.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
for sim in ('1', '2', '4'):
    open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=%s\nXyceExecutable=/usr/bin/true\nSpiceOpusExecutable=/usr/bin/true\n' % sim)
    ws = HERE + '/ws_src' + sim; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
    s = Server(ws)
    s.call('new_document', {})
    s.call('batch', {'calls': [
      {'tool': 'add_component', 'arguments': {'type': 'Vac', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '0', 'ACmag': '1 V'}}},
      {'tool': 'add_component', 'arguments': {'type': 'Iac', 'name': 'I1', 'x': 300, 'y': 200, 'properties': {'ACmag': '2 mA'}}},
      {'tool': 'add_component', 'arguments': {'type': 'Vpulse', 'name': 'V2', 'x': 500, 'y': 200}},
      {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 700, 'y': 200}},
      {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
      {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'I1.1', 'to': 'R1.1'}},
      {'tool': 'connect', 'arguments': {'from': 'I1.2', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'V2.1', 'to': 'ground'}},
      {'tool': 'add_analysis', 'arguments': {'kind': 'ac'}}]}, ok=False)
    net = s.call('get_netlist', {}, ok=False); net = net if isinstance(net, str) else json.dumps(net)
    print(sim, [l.strip()[:110] for l in net.splitlines() if l[:2] in ('V1', 'I1', 'V2')])
    s.close()

# The tools: a ground on a net, then a label on one of its wires (or the other way round) - one net, two nodes?
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_split2q'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=8\n')
s = Server(ws)
def nl():
    net = s.call('get_netlist', {}, ok=False)
    net = net if isinstance(net, str) else json.dumps(net)
    return [l for l in net.splitlines() if l.strip() and not l.startswith('#') and ('R1' in l or 'R2' in l or 'V1' in l)] or net[:300]
    return [l for l in net.splitlines() if l.strip()[:4] in ('Vdc:', 'R:R1', 'R:R2') or l.strip().startswith('Vdc') or l.strip().startswith('R:')]
def build(order):
    s.call('new_document', {})
    s.call('batch', {'calls': [
      {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200}},
      {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 300, 'y': 200}},
      {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 500, 'y': 200}},
      {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R2.1', 'to': 'R1.1'}},
      {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}},
      {'tool': 'add_analysis', 'arguments': {'kind': 'op'}}]})
    w = [x for x in s.call('get_schematic', {})['wires']]
    # The wire between R1.2 and R2.2: its middle.
    sch = s.call('get_schematic', {})
    pins = {c['name']: {str(p['pin']): (p['x'], p['y']) for p in c['pins']} for c in sch['components'] if 'pins' in c}
    a, b = pins['R1']['2'], pins['R2']['2']
    seg = [x for x in w if (x['from'] if 'from' in x else x.get('x1')) is not None][:1]
    steps = []
    if order == 'ground first':
        steps.append(('connect', {'from': 'V1.2', 'to': 'ground'}))
        steps.append(('connect', {'from': 'V1.2', 'to': 'R1.2'}))
        steps.append(('set_label', {'at': list(a), 'name': 'foo'}))
    else:
        steps.append(('connect', {'from': 'V1.2', 'to': 'R1.2'}))
        steps.append(('set_label', {'at': list(a), 'name': 'foo'}))
        steps.append(('connect', {'from': 'R2.2', 'to': 'ground'}))
    for tool, args in steps:
        r = s.call(tool, args, ok=False)
        print('   ', tool, json.dumps(args), '->', (r if isinstance(r, str) else json.dumps(r))[:150])
    chk = s.call('check_schematic', {}, ok=False)
    print(order, '|', nl(), '| check:', [i['message'][:80] for k in ('errors', 'warnings') for i in (chk.get(k, []) if isinstance(chk, dict) else [])])
build('ground first'); build('label first')
s.close()

# set_subcircuit_parameters: names and values ngspice may not take, simulated.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_sub'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
s = Server(ws)
def c(tool, args):
    try: return s.call(tool, args)
    except RuntimeError as e: return 'ERROR ' + str(e)[:300]
c('new_document', {})
c('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
  {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P2', 'x': 100, 'y': 300}},
  {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 200, 'properties': {'R': '{Rs}'}}},
  {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'P2.1', 'to': 'R1.2'}},
  {'tool': 'save_document', 'arguments': {'as': 'sub.sch'}}]})
print('set Rs', json.dumps(c('set_subcircuit_parameters', {'path': 'sub.sch', 'parameters': ['Rs=1k']}))[:300])
c('save_document', {'path': 'sub.sch'})
c('new_document', {})
c('batch', {'calls': [
  {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1'}}},
  {'tool': 'add_component', 'arguments': {'type': 'Sub', 'name': 'SUB1', 'x': 300, 'y': 200, 'properties': {'File': 'sub.sch'}}},
  {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'SUB1.1'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
  {'tool': 'connect', 'arguments': {'from': 'SUB1.2', 'to': 'ground'}},
  {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
  {'tool': 'save_document', 'arguments': {'as': 'top.sch'}}]})
def run(label):
    r = c('simulate', {'path': 'top.sch'})
    net = c('get_netlist', {'path': 'top.sch'})
    sub = [l for l in (net if isinstance(net, str) else json.dumps(net)).splitlines() if 'SUBCKT' in l.upper() or l.startswith('XSUB')]
    print('==', label, '| succeeded:', r.get('succeeded') if isinstance(r, dict) else r[:200], '| errors:', (r.get('errors') if isinstance(r, dict) else '')[:2], '|', sub)
run('Rs=1k')
for p in (['temp=27'], ['time=1'], ['R1=5'], ['Rs=1,5'], ['Rs=1k;'], ['x=1', 'X=2'], ['Rs=-'], ['v=1e3'], ['Rs=1k', 'k={2*Rs}']):
    r = c('set_subcircuit_parameters', {'path': 'sub.sch', 'parameters': p, 'replace': True})
    if isinstance(r, str): print('==', p, '->', r[:200]); continue
    if 'Rs' not in ' '.join(p): c('set_subcircuit_parameters', {'path': 'sub.sch', 'parameters': ['Rs=1k'] })
    c('save_document', {'path': 'sub.sch'})
    run(str(p))
s.close()

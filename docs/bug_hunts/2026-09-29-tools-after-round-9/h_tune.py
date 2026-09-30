from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh7'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
s.call('new_document', {})
s.call('batch', {'atomic': True, 'calls': [
 {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '5 V'}}},
 {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 150, 'properties': {'R': '1k'}}},
 {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R2', 'x': 400, 'y': 200, 'rotation': 1, 'properties': {'R': '1k'}}},
 {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'R2.2'}},
 {'tool': 'connect', 'arguments': {'from': 'R2.1', 'to': 'ground'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
 {'tool': 'set_label', 'arguments': {'at': 'R1.2', 'name': 'mid'}}, {'tool': 'set_label', 'arguments': {'at': 'V1.1', 'name': 'top'}},
 {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
 {'tool': 'save_document', 'arguments': {'as': WS + '/t.sch'}}]})
base = {'path': 't.sch', 'component': 'R1', 'measure': {'operating_point': 'mid'}, 'target': 2.0, 'range': ['100', '10k']}
cases = {
 'empty hold': {'hold': []},
 'min>max': {'hold': [{'measure': {'operating_point': 'top'}, 'min': 6, 'max': 4}]},
 'no bounds': {'hold': [{'measure': {'operating_point': 'top'}}]},
 'unknown node': {'hold': [{'measure': {'operating_point': 'nosuch'}, 'min': 1}]},
 'ac measure in op tune': {'hold': [{'measure': {'variable': 'ac.v(mid)', 'what': 'bandwidth'}, 'min': 1}]},
 'compare alone': {'compare': True},
 'hold not list': {'hold': {'measure': {'operating_point': 'top'}, 'min': 1}},
 'nan min': {'hold': [{'measure': {'operating_point': 'top'}, 'min': 'abc'}]},
 'hold ok': {'hold': [{'measure': {'operating_point': 'top'}, 'min': 4.9}], 'compare': True},
 'unkept hold': {'hold': [{'measure': {'operating_point': 'mid'}, 'max': 1.0}]},
}
for name, extra in cases.items():
    r = s.call('tune', dict(base, **extra), ok=False)
    if isinstance(r, str): print('%-22s ERR %s' % (name, r[:170].replace('\n', ' '))); continue
    print('%-22s %s | set=%s | held back=%s | b&a=%s | runs=%d' % (name, r.get('within tolerance'), str(r.get('set'))[:40], str(r.get('held back'))[:80], 'before and after' in r, len(r.get('runs', []))))
    s.call('undo', {'path': 't.sch'}, ok=False)
    v = [p['value'] for c in s.call('get_schematic', {'path': 't.sch'})['components'] if c['name'] == 'R1' for p in c['properties'] if p['name'] == 'R']
    if v != ['1k']: print('    R1 now', v); s.call('edit_component', {'path': 't.sch', 'name': 'R1', 'properties': {'R': '1k'}})
s.close()
s = Server(WS)
s.call('open_document', {'path': WS + '/t.sch'})
r = s.call('tune', dict(base, hold=[{'measure': {'operating_point': 'nosuch'}, 'min': 1}]), ok=False)
print(json.dumps(r, indent=1)[:1500])
r = s.call('tune', dict(base, measure={'operating_point': 'nosuch'}), ok=False)
print('target unknown:', json.dumps(r)[:400])
s.close()

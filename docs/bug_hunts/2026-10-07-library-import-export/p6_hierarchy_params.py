"""p6: library parts of a hierarchy (a subcircuit placing another) and with parameters; each simulated under ngspice and Qucsator."""
import os, re, json, mcp, lib
s = mcp.Server('p6')
d = lib.project(s, 'hp', {'inner.sch': lib.divider('1k', '1k'), 'par.sch': lib.divider('1k', '{r2}')})
s.call('open_document', {'path': d + '/par.sch'})
print('params:', json.dumps(s.call('set_subcircuit_parameters', {'parameters': ['r2=1k']}))[:200])
s.call('save_document', {})
s.call('close_document', {})

def wrapper(name, file, props=None):
    s.call('new_document', {})
    s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
    s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 600, 'y': 100})
    s.call('add_component', {'type': 'Sub', 'name': 'SUB1', 'x': 320, 'y': 120, 'properties': dict({'File': file}, **(props or {}))})
    s.call('connect', {'from': 'P1.1', 'to': 'SUB1.1'})
    s.call('connect', {'from': 'SUB1.2', 'to': 'P2.1'})
    s.call('save_document', {'as': d + '/' + name})
    s.call('close_document', {})

wrapper('outer.sch', 'inner.sch')
wrapper('outer2.sch', 'par.sch', {'r2': '3k'})
wrapper('outer3.sch', 'outer.sch')         # two levels down
r = s.call('create_library', {'name': 'Hier'})
print('create:', s.last_error, [p['part'] for p in r.get('parts', [])], [m for m in r.get('messages', []) if 'rror' in m or 'ARN' in m])
want = {'inner': 0.5, 'outer': 0.5, 'outer2': 0.75, 'outer3': 0.5, 'par': 0.5}
parts = {p['part']: p['place'] for p in r.get('parts', [])}
for simulator in ['ngspice', 'qucsator']:
    s.call('set_simulator', {'simulator': simulator})
    for part, place in parts.items():
        out = lib.bench(s, place, d + f'/use_{part}_{simulator}.sch')
        v = re.search(r'"value": ([-0-9.e]+)', out.get('v(out)', '')) if 'v(out)' in out else None
        print(f'  {simulator} {part} want {want.get(part)}:', v.group(1) if v else json.dumps(out)[:400])
    # A parameter set on the placed part.
    place = dict(parts['par'])
    place = {'type': place['type'], 'properties': dict(place['properties'], r2='3k')}
    out = lib.bench(s, place, d + f'/use_par3k_{simulator}.sch')
    v = re.search(r'"value": ([-0-9.e]+)', out.get('v(out)', '')) if 'v(out)' in out else None
    print(f'  {simulator} par with r2=3k want 0.75:', v.group(1) if v else json.dumps(out)[:400])
s.close()

"""p6c: two libraries whose parts each place a subcircuit named inner (different ones), in one circuit; and with the user's own inner.sch.
The first-line loss (p6b) worked around: a newline put after <Spice> in the library files."""
import os, re, json, mcp, lib
s = mcp.Server('p6c')

def vals(names):
    g = s.call('get_dataset', {'variables': names})
    return json.dumps(g.get('variables'))[:400] if isinstance(g, dict) else str(g)[:400]

def library(proj, libname, r2):
    d = lib.project(s, proj, {'inner.sch': lib.divider('1k', r2)})
    s.call('new_document', {})
    s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
    s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 600, 'y': 100})
    s.call('add_component', {'type': 'Sub', 'name': 'SUB1', 'x': 320, 'y': 120, 'properties': {'File': 'inner.sch'}})
    s.call('connect', {'from': 'P1.1', 'to': 'SUB1.1'})
    s.call('connect', {'from': 'SUB1.2', 'to': 'P2.1'})
    s.call('save_document', {'as': d + '/outer.sch'})
    s.call('close_document', {})
    r = s.call('create_library', {'name': libname, 'subcircuits': ['outer']})
    f = s.ws + f'/user_lib/{libname}.lib'
    text = open(f).read()
    open(f, 'w').write(text.replace('<Spice>.SUBCKT', '<Spice>\n.SUBCKT'))
    return r['parts'][0]['place'], d

pa, _ = library('a', 'LA', '1k')     # 0.5
pb, _ = library('b', 'LB', '3k')     # 0.75
d = lib.project(s, 'use', {'inner.sch': lib.divider('3k', '1k')})   # the user's own inner: 0.25
s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
for i, (pl, y) in enumerate([(pa, 120), (pb, 320)], 1):
    print('  place', pl, str(s.call('add_component', {'type': pl['type'], 'name': f'X{i}', 'x': 360, 'y': y, 'properties': pl['properties']}))[:300])
    s.call('connect', {'from': 'V1.1', 'to': f'X{i}.1'})
    s.call('set_label', {'at': f'X{i}.2', 'name': f'out{i}'})
s.call('connect', {'from': 'V1.2', 'to': 'ground'})
s.call('add_analysis', {'kind': 'op'})
s.call('save_document', {'as': d + '/two.sch'})
sim = s.call('simulate', {})
print('LA and LB together:', sim.get('succeeded'), vals(['out1', 'out2']) if sim.get('succeeded') else json.dumps(sim.get('errors'))[:500], '(want 0.5, 0.75)')
print('  warnings:', json.dumps(sim.get('warnings'))[:400])
# With the user's own inner.sch beside it.
s.call('add_component', {'type': 'Sub', 'name': 'SUB9', 'x': 360, 'y': 520, 'properties': {'File': 'inner.sch'}})
s.call('connect', {'from': 'V1.1', 'to': 'SUB9.1'})
s.call('set_label', {'at': 'SUB9.2', 'name': 'out3'})
s.call('save_document', {})
sim = s.call('simulate', {})
print('with the own inner.sch:', sim.get('succeeded'), vals(['out1', 'out2', 'out3']) if sim.get('succeeded') else json.dumps(sim.get('errors'))[:500], '(want 0.5, 0.75, 0.25)')
print(s.call('get_netlist', {}))
print('qucsator:', s.call('set_simulator', {'simulator': 'qucsator'}))
sim = s.call('simulate', {})
print('qucsator, all three:', sim.get('succeeded'), vals(['out1', 'out2', 'out3']) if sim.get('succeeded') else json.dumps(sim.get('errors'))[:500], '(want 0.5, 0.75, 0.25)')
s.close()

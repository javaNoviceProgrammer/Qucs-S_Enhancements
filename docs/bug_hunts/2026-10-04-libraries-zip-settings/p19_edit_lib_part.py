"""p19: edit_component on a wired library part: Lib to a library not there, Comp to a part of other pins, back; undo."""
import mcp, json
s = mcp.Server('p19')
s.call('new_document', {})
s.call('batch', {'calls': [
    {'tool': 'add_component', 'arguments': {'type': 'Lib', 'name': 'U1', 'x': 300, 'y': 200, 'properties': {'Lib': 'OpAmps', 'Comp': 'uA741'}}},
    {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 100, 'y': 200}},
    {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'U1.1'}},
    {'tool': 'connect', 'arguments': {'from': 'U1.3', 'to': 'ground'}}]})
print('batch:', str(_b)[:300]) if False else None
def nets():
    m = s.call('get_netlist', {'map': True})
    return {k: sorted(v) for k, v in m.get('nodes', {}).items()} if isinstance(m, dict) else m
def pins():
    g = s.call('get_schematic', {'components': ['U1']})
    return len(g['components'][0].get('pins', [])) if isinstance(g, dict) and g.get('components') else g
n0 = nets(); print('start pins', pins(), 'nets', n0)
for props in ({'Lib': 'NoSuchLibrary'}, {'Comp': 'NoSuchPart'}, {'Lib': 'Ideal', 'Comp': 'VSum'}, {'Comp': '1N4148', 'Lib': 'Diodes'}, {'Lib': 'OpAmps', 'Comp': 'uA741'}):
    r = s.call('edit_component', {'name': 'U1', 'properties': props})
    print(json.dumps(props), '-> error', s.last_error, '| pins', pins(), '|', str(r)[:200])
    print('    nets', nets())
r = s.call('undo', {}); print('undo:', str(r)[:100], 'pins', pins())
print('alive', 'workspace' in s.call('get_state', {}))
s.close()

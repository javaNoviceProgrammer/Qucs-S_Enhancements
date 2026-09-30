from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh8'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
def fresh():
    s.call('new_document', {})
    s.call('batch', {'atomic': True, 'calls': [
     {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200}},
     {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 250, 'y': 150}},
     {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'ground'}},
     {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}}, {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
     {'tool': 'save_document', 'arguments': {'as': WS + '/ty.sch'}}]})
fresh()
probes = [
 ('arrange', {'feedback': 1}), ('arrange', {'feedback': 'BELOW '}), ('arrange', {'straighten': 'yes', 'preview': True}), ('arrange', {'straighten': 1, 'preview': True}),
 ('arrange', {'keep_places': 'true', 'preview': True}), ('arrange', {'supplies': ['labels'], 'preview': True}),
 ('check_schematic', {'subcircuits': 'yes'}), ('check_schematic', {'subcircuits': 1}),
 ('describe_part', {'library': 5, 'part': 'uA741'}), ('describe_part', {'library': 'OpAmps', 'part': ['uA741']}),
 ('import_netlist', {'text': 'R1 a 0 1k\n.end', 'title': 5}), ('import_netlist', {'text': 'R1 a 0 1k\n.end', 'title': ['x']}),
 ('make_symbol', {'sides': ['in']}), ('make_symbol', {'sides': {'in': 5}}),
 ('get_netlist', {'map': 'true'}), ('get_netlist', {'map': 1}),
 ('create_subcircuit', {'names': 'R1', 'save_as': 'c1.sch', 'preview': True}),
 ('edit_diagram', {'notation': 5}),
]
for tool, args in probes:
    r = s.call(tool, args, ok=False)
    t = r if isinstance(r, str) else json.dumps(r)
    print('%-18s %-45s -> %s' % (tool, json.dumps(args)[:45], t[:130].replace('\n', ' ')))
s.close()

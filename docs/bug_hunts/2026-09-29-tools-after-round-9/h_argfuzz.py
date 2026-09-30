import json, os, sys, random, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
WS = HERE + '/wsaf'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
vals = [None, True, False, 0, 1, -1, 2**31, -2**31, 1e308, 0.5, '', ' ', 'x', 'below', 'above', 'inline', 'labels', 'diagram', 'scientific',
        'power_of_ten', 'OpAmps', 'uA741', 'ua741(TI)', 'R1', 'U1', 'X1', 'V1', 'ground', 'b.sch', '../x.sch', [], [1], ['R1'], ['R1', 'V1'], {}, {'in': 'left'},
        {'x': 1}, 'a' * 5000, '\u0000', 'é漢字', '{"a":1}', 199, 200, 201]
tools = {'describe_part': ['library', 'part'], 'arrange': ['feedback', 'straighten', 'keep_places', 'supplies', 'wire_labels', 'preview', 'spacing'],
         'create_subcircuit': ['names', 'save_as', 'name', 'replace', 'preview'], 'make_symbol': ['sides'], 'check_schematic': ['subcircuits'],
         'edit_marker': ['marker', 'notation', 'precision', 'format', 'at'], 'edit_diagram': ['notation', 'decimals', 'diagram'],
         'add_marker': ['at', 'notation', 'trace'], 'get_netlist': ['map'], 'import_netlist': ['text', 'title', 'save_as', 'replace'],
         'replace_component': ['name', 'type', 'pins']}
def setup(s):
    s.call('new_document', {}, ok=False)
    s.call('batch', {'calls': [
     {'tool': 'add_component', 'arguments': {'type': 'Lib', 'name': 'U1', 'x': 400, 'y': 300, 'properties': {'Lib': 'OpAmps', 'Comp': 'ua741(TI)'}}},
     {'tool': 'add_component', 'arguments': {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 300}},
     {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 450, 'y': 150}},
     {'tool': 'connect', 'arguments': {'from': 'V1.1', 'to': 'U1.INP'}}, {'tool': 'connect', 'arguments': {'from': 'V1.2', 'to': 'ground'}},
     {'tool': 'connect', 'arguments': {'from': 'R1.1', 'to': 'U1.INN'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'U1.OUT'}},
     {'tool': 'add_analysis', 'arguments': {'kind': 'op'}},
     {'tool': 'add_diagram', 'arguments': {'traces': ['v(out)']}},
     {'tool': 'save_document', 'arguments': {'as': WS + '/b%d.sch' % random.randrange(10**6)}}]}, ok=False)
s = Server(WS); setup(s); t0 = time.time(); n = 0; deaths = []
while time.time() - t0 < budget:
    tool = random.choice(list(tools)); args = {}
    for k in random.sample(tools[tool], random.randint(0, len(tools[tool]))): args[k] = random.choice(vals)
    if random.random() < 0.2: args['max_chars'] = random.choice([200, 250, 1000, 'x', -1])
    n += 1
    try:
        s.call(tool, args, ok=False)
        if n % 40 == 0: setup(s)
    except Exception as e:
        deaths.append((tool, json.dumps(args, default=str)[:200], repr(e)[:80]))
        try: s.close()
        except Exception: pass
        s = Server(WS); setup(s)
try: s.close()
except Exception: pass
print(n, 'calls,', len(deaths), 'server deaths')
for d in deaths[:8]: print(d)

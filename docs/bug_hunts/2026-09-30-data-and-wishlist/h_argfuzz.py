# Random calls with junk arguments to the tools of the wishlist, rounds 10-11 and import_data.
import json, os, sys, random, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
WS = HERE + '/wsaf'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
open(WS + '/d.csv', 'w').write('t,a,b\n0,1,2\n1,2,3\n2,4,1\n')
open(WS + '/w.txt', 'w').write('# f g\n1 2\n2 3\n')
vals = [None, True, False, 0, 1, -1, 2**31, 1e308, 0.5, '', ' ', 'x', 'd.csv', 'w.txt', 'd', 'w', 'd_2', '#row', 'row', 't', 'a', 'A',
        'Sheet1', 'b.sch', 'b.dpl', '../x.csv', '/etc/passwd', 'data_display', 'schematic', 'd:a', 'ngspice/d:a', 'qucsator/d:a', 'v(out)',
        [], [1], ['d:a'], ['d:a', 'd:b'], [{'variable': 'd:a'}], ['Rs=1k'], [{'name': 'Rs', 'default': '1k'}], [{'name': 'File'}], ['R s=1'],
        {}, {'x': 1}, 'a' * 5000, '\u0000', 'é漢字', 'X', 'Y']
tools = {'import_data': ['file', 'path', 'name', 'x', 'sheet', 'reload', 'remove'],
         'set_subcircuit_parameters': ['path', 'parameters', 'remove', 'replace', 'preview'],
         'make_symbol': ['path', 'sides', 'parameters', 'prefix'],
         'add_diagram': ['path', 'document', 'traces', 'preview', 'type'],
         'add_trace': ['diagram', 'variable'], 'edit_trace': ['diagram', 'trace', 'variable'],
         'get_dataset': ['path', 'variables'], 'list_documents': ['kind', 'folder', 'search'],
         'clean_scratch': ['path', 'datasets'], 'copy_document': ['path', 'to', 'replace'],
         'reload_data': ['path'], 'get_netlist': ['path', 'last', 'max_chars']}
def setup(s):
    s.call('new_document', {}, ok=False)
    s.call('batch', {'calls': [
     {'tool': 'add_component', 'arguments': {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100}},
     {'tool': 'add_component', 'arguments': {'type': 'R', 'name': 'R1', 'x': 200, 'y': 100}},
     {'tool': 'connect', 'arguments': {'from': 'P1.1', 'to': 'R1.1'}}, {'tool': 'connect', 'arguments': {'from': 'R1.2', 'to': 'ground'}},
     {'tool': 'add_diagram', 'arguments': {'traces': ['d:a']}},
     {'tool': 'save_document', 'arguments': {'as': WS + '/b.sch', 'replace': True}}]}, ok=False)
s = Server(WS); setup(s); t0 = time.time(); n = 0; deaths = []
while time.time() - t0 < budget:
    tool = random.choice(list(tools)); args = {}
    for k in random.sample(tools[tool], random.randint(0, len(tools[tool]))): args[k] = random.choice(vals)
    n += 1
    try:
        s.call(tool, args, ok=False)
        if n % 40 == 0: setup(s)
    except Exception as e:
        deaths.append((tool, json.dumps(args, default=str)[:300], repr(e)[:80]))
        try: s.close()
        except Exception: pass
        s = Server(WS); setup(s)
try: s.close()
except Exception: pass
print(n, 'calls,', len(deaths), 'server deaths')
for d in deaths[:8]: print(d)

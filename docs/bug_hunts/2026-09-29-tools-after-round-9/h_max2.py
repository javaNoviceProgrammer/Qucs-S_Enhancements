from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh5'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
def raw(tool, args):
    m = s.rpc('tools/call', {'name': tool, 'arguments': args})
    r = m.get('result', m)
    return '\n'.join(c.get('text', '') for c in r.get('content', []))
s.call('new_document', {})
s.call('batch', {'calls': [{'tool': 'add_component', 'arguments': {'type': 'R', 'x': 100 + 60 * (i % 30), 'y': 100 + 80 * (i // 30)}} for i in range(300)]})
for tool, args in [('get_schematic', {}), ('get_state', {}), ('list_component_types', {}), ('get_netlist', {'map': True}), ('check_schematic', {})]:
    res = []
    for v in [200, 300, 500, 800, 1200, 2000, 4000]:
        t = raw(tool, dict(args, max_chars=v))
        try: json.loads(t); ok = 'json'
        except Exception: ok = 'TEXT'
        res.append('%d:%d%s' % (v, len(t), '' if ok == 'json' else '!'))
    print(tool, ' '.join(res))
print(raw('get_schematic', {'max_chars': 300})[-200:])
s.close()

from mcp import Server, HERE
import json, shutil
WS = HERE + '/wsh4'; shutil.rmtree(WS, ignore_errors=True)
s = Server(WS)
def raw(tool, args):
    m = s.rpc('tools/call', {'name': tool, 'arguments': args})
    r = m.get('result', m)
    t = '\n'.join(c.get('text', '') for c in r.get('content', [])) if isinstance(r, dict) else str(r)
    return r.get('isError') if isinstance(r, dict) else None, t
s.call('new_document', {})
s.call('batch', {'calls': [{'tool': 'add_component', 'arguments': {'type': 'R', 'x': 100 + 60 * (i % 30), 'y': 100 + 80 * (i // 30)}} for i in range(300)]})
for v in [200, 199, 200.0, 200.5, '300', -5, 1e12, 2**31, 2**63, True, None, [300]]:
    err, t = raw('get_schematic', {'max_chars': v})
    ok = True
    try: j = json.loads(t)
    except Exception: j = None; ok = False
    print(repr(v), 'err' if err else 'ok', len(t), 'json' if ok else 'TEXT', (j.get('trimmed', '')[:70] if isinstance(j, dict) else t[:120]))
# batch with max_chars inside a call and outside
err, t = raw('batch', {'calls': [{'tool': 'get_schematic', 'arguments': {'max_chars': 300}}], 'max_chars': 500})
print('batch', err, len(t), t[:200])
err, t = raw('get_netlist', {'max_chars': 300})
print('netlist', err, len(t), t[-150:])
err, t = raw('describe_format', {'max_chars': 250})
print('describe_format', err, len(t), repr(t[-80:]))
err, t = raw('open_document', {'path': '/nonexistent/x.sch', 'max_chars': 200})
print('error', err, len(t), t[:150])
err, t = raw('screenshot', {'max_chars': 200})
print('screenshot', err, len(t), t[:100])
s.close()

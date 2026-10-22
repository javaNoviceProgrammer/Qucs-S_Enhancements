import sys, json, time; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
sys.path.insert(0, '/tmp/qucs-hunt')
from mcp_client import Client
from probe import H
from f2 import well, spec
m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 2e6, 4e6, 6e6, 8e6, 1e7", "V/m"]]}, dict(spec, states=12)], {"F": "F", "h1d": "0.1[nm]"})
path = H + '/f13.qfem'; json.dump(m, open(path, 'w'))
c = Client(H + '/f13.w')
c.call('fem_model', {'action': 'open', 'path': path})
def send(i, name, args):
    c._send({'jsonrpc': '2.0', 'id': i, 'method': 'tools/call', 'params': {'name': name, 'arguments': args}})
t0 = time.time()
send(101, 'fem_solve', {'path': path, 'study': 's'})
send(102, 'fem_solve', {'path': path, 'study': 's'})
send(103, 'fem_build', {'path': path, 'what': 'mesh'})
send(104, 'fem_stop', {'path': path})
send(105, 'fem_solve', {'path': path, 'study': 's'})
send(106, 'fem_evaluate', {'path': path, 'study': 's', 'expression': 'qw.Ee1'})
got = {}
while len(got) < 6 and time.time() - t0 < 120:
    line = c.p.stdout.readline()
    if not line: break
    try: msg = json.loads(line)
    except Exception: continue
    if msg.get('id') in (101, 102, 103, 104, 105, 106):
        r = msg.get('result', {})
        text = ''.join(x.get('text', '') for x in r.get('content', []))
        got[msg['id']] = (round(time.time() - t0, 2), r.get('isError'), (lambda t: (lambda j: {k: (v if k != 'log' else len(v)) for k, v in j.items()} if isinstance(j, dict) else t[:300])(json.loads(t) if t.startswith('{') else t))(text))
for k in sorted(got, key=lambda k: got[k][0]): print(k, got[k])
c.close()

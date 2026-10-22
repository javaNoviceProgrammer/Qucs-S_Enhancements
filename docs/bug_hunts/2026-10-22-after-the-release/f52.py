import sys, json, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S); sys.path.insert(0, S + '/hunt2')
from mcp_client import Client
from probe import H
from f2 import well, spec
APP = sys.argv[1] if len(sys.argv) > 1 else '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 2e6, 4e6, 6e6", "V/m"]]}, dict(spec, states=6, energies="1.45[eV], 1.5[eV]")], {"F": "F", "h1d": "0.25[nm]"})
path = H + '/f52.qfem'; json.dump(m, open(path, 'w'))
err = H + '/f52.err'
os.environ['MCP_STDERR'] = err
c = Client(H + '/f52.w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1'})
c.call('fem_model', {'action': 'open', 'path': path})
c._send({'jsonrpc': '2.0', 'id': 301, 'method': 'tools/call', 'params': {'name': 'fem_solve', 'arguments': {'path': path, 'study': 's'}}})
time.sleep(1.5)
c.p.stdin.close()   # (the client goes: the app quits with the solve waited for)
try: rc = c.p.wait(timeout=120)
except Exception: c.p.kill(); rc = 'killed'
print('exit', rc)
t = open(err, errors='replace').read()
print([l[:220] for l in t.splitlines() if 'ERROR:' in l or 'SUMMARY' in l or 'runtime error' in l][:4])

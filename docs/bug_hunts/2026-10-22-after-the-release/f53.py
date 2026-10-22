import sys, json, time, os, shutil
S = '/tmp/qucs-hunt'
sys.path.insert(0, S); sys.path.insert(0, S + '/hunt2')
from mcp_client import Client
from probe import H
from f2 import well, spec
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
which = sys.argv[1]
err = H + '/f53_%s.err' % which
os.environ['MCP_STDERR'] = err
c = Client(H + '/f53.w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1'})
if which == 'build':
    shutil.copy(H + '/kfr/flow.qfem', H + '/f53flow.qfem'); path = H + '/f53flow.qfem'
    c.call('open_document', {'path': path})
    c._send({'jsonrpc': '2.0', 'id': 401, 'method': 'tools/call', 'params': {'name': 'fem_build', 'arguments': {'path': path, 'what': 'mesh', 'timeout': 300}}})
else:
    m = well([{"name": "F", "expression": "0[V/m]"}], [{"type": "sweep", "parameters": [["F", "0, 2e6, 4e6, 6e6", "V/m"]]}, dict(spec, states=6, energies="1.45[eV], 1.5[eV]")], {"F": "F", "h1d": "0.25[nm]"})
    path = H + '/f53.qfem'; json.dump(m, open(path, 'w'))
    c.call('fem_model', {'action': 'open', 'path': path})
    c._send({'jsonrpc': '2.0', 'id': 402, 'method': 'tools/call', 'params': {'name': 'fem_solve', 'arguments': {'path': path, 'study': 's', 'timeout': 1}}})
    time.sleep(2)
    c._send({'jsonrpc': '2.0', 'id': 403, 'method': 'tools/call', 'params': {'name': 'fem_stop', 'arguments': {'path': path, 'timeout': 60}}})
time.sleep(2)
c.p.stdin.close()
try: rc = c.p.wait(timeout=180)
except Exception: c.p.kill(); rc = 'killed'
t = open(err, errors='replace').read()
print(which, 'exit', rc, [l[:200] for l in t.splitlines() if 'ERROR:' in l or 'SUMMARY' in l][:3])

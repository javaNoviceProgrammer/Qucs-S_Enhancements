import sys, json, time, os, shutil
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
H = S + '/hunt2'
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
shutil.copy(H + '/kfr/flow.qfem', H + '/f56flow.qfem'); path = H + '/f56flow.qfem'
err = H + '/f56.err'; os.environ['MCP_STDERR'] = err
c = Client(H + '/f56.w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1'})
c.call('open_document', {'path': path})
print(c.call('fem_build', {'path': path, 'what': 'geometry', 'timeout': 1})[1][:120])
c._send({'jsonrpc': '2.0', 'id': 701, 'method': 'tools/call', 'params': {'name': 'fem_describe', 'arguments': {'path': path}}})
time.sleep(2)
t = time.time(); c.p.stdin.close()
try: rc = c.p.wait(timeout=400)
except Exception: c.p.kill(); rc = 'killed'
s = open(err, errors='replace').read()
print('exit', rc, '%.1f s' % (time.time() - t), [l[:200] for l in s.splitlines() if 'ERROR:' in l or 'SUMMARY' in l][:3])

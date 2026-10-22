import sys, json, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/simq'
APP = sys.argv[1] if len(sys.argv) > 1 else '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
err = D + '/err.txt'; os.environ['MCP_STDERR'] = err
c = Client(D + '/w', app=APP, env_extra={'ASAN_OPTIONS': 'detect_leaks=0:detect_stack_use_after_return=1'})
print(c.call('open_document', {'path': D + '/amp.sch'})[1][:100])
c._send({'jsonrpc': '2.0', 'id': 601, 'method': 'tools/call', 'params': {'name': 'simulate', 'arguments': {'path': D + '/amp.sch', 'timeout': 300}}})
time.sleep(1.0)
t = time.time(); c.p.stdin.close()
try: rc = c.p.wait(timeout=240)
except Exception: c.p.kill(); rc = 'killed'
s = open(err, errors='replace').read()
print('exit', rc, '%.1f s' % (time.time() - t), [l[:200] for l in s.splitlines() if 'ERROR:' in l or 'SUMMARY' in l][:3])

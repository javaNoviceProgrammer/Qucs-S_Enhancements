import json, subprocess, sys, time, os
app, w = sys.argv[1], sys.argv[2]
env = dict(os.environ, HOME=w+'/home', QUCS_SETTINGS_DIR=w+'/settings', QUCS_CACHE_DIR=w+'/cache', QUCS_TRASH_DIR=w+'/trash', QUCS_LIBRARY_DIR=w+'/lib', QT_QPA_PLATFORM='offscreen')
p = subprocess.Popen([app, '--mcp-server', '--workspace', w+'/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
n = 0
def rpc(method, params):
    global n; n += 1
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': n, 'method': method, 'params': params}) + '\n'); p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line: return None
        r = json.loads(line)
        if r.get('id') == n: return r
rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'probe', 'version': '1'}})
for kind in ['python', 'terminal']:
    for args in [{'kind': kind}, {'kind': kind, 'interrupt': True, 'wait': 8}]:
        t = time.time()
        r = rpc('tools/call', {'name': 'console', 'arguments': args})
        txt = r['result']['content'][0]['text'] if r and 'result' in r else str(r)
        print(f'{json.dumps(args)}: {time.time()-t:.1f} s -> {txt[:160]!r}')
p.kill()

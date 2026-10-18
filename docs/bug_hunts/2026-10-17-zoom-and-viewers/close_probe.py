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
        if not line: return 'EOF'
        r = json.loads(line)
        if r.get('id') == n: return ' '.join(c.get('text', '') for c in r.get('result', {}).get('content', [])) if 'result' in r else json.dumps(r)
rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'probe', 'version': '1'}})
for f in sys.argv[3:]:
    path = w + '/ws/' + f
    rpc('tools/call', {'name': 'open_document', 'arguments': {'path': path}})
    t = time.time(); r = rpc('tools/call', {'name': 'fem_solve', 'arguments': {'path': path, 'timeout': 1}}); ts = time.time() - t
    t = time.time(); c = rpc('tools/call', {'name': 'close_document', 'arguments': {'path': path, 'unsaved': 'discard'}}); tc = time.time() - t
    print(f'{f}: solve {ts:.1f} s ({r[:120]!r}); close {tc:.2f} s')
p.kill()

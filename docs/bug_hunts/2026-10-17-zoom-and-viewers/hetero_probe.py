import json, subprocess, sys, time, os, copy
app, w, src = sys.argv[1], sys.argv[2], sys.argv[3]
base = json.load(open(src))
base['studies'][0]['steps'] = [{"type": "sweep", "parameters": [["Va", "0.5, 0.7", "V"]]}, {"type": "stationary"}]
def variant(name, materials, het=None):
    m = copy.deepcopy(base)
    c = m['components'][0]
    if materials is not None: c['materials'] = materials
    if het is not None: c['physics'][0]['heterojunctions'] = het
    path = f'{w}/ws/{name}.qfem'
    json.dump(m, open(path, 'w'), indent=1)
    return path
two = [{"material": "Silicon", "selection": {"only": ["pside"]}}, {"material": "Silicon", "selection": {"only": ["nside"]}}]
files = [variant('v0_one', None), variant('v1_two', two), variant('v2_two_abrupt', two, 'abrupt'), variant('v3_two_graded', two, 'element'),
         variant('v4_one_graded', None, 'element')]
env = dict(os.environ, HOME=w+'/home', QUCS_SETTINGS_DIR=w+'/settings', QUCS_CACHE_DIR=w+'/cache', QUCS_TRASH_DIR=w+'/trash', QUCS_LIBRARY_DIR=w+'/lib', QT_QPA_PLATFORM='offscreen')
p = subprocess.Popen([app, '--mcp-server', '--workspace', w+'/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
n = 0
def call(name, args):
    global n; n += 1
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': n, 'method': 'tools/call', 'params': {'name': name, 'arguments': args}}) + '\n'); p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line: return 'EOF'
        r = json.loads(line)
        if r.get('id') == n:
            return ' '.join(c.get('text', '') for c in r.get('result', {}).get('content', [])) if 'result' in r else json.dumps(r)
n += 1
p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': n, 'method': 'initialize', 'params': {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'probe', 'version': '1'}}}) + '\n'); p.stdin.flush(); p.stdout.readline()
for f in files:
    name = os.path.basename(f)
    print('==', name, call('open_document', {'path': f})[:100])
    t = time.time()
    r = call('fem_solve', {'path': name})
    print('   solve %.1f s:' % (time.time() - t), r[:300].replace('\n', ' '))
    for e in ['semi.I_1', 'semi.I_2']:
        print('  ', e, call('fem_evaluate', {'path': name, 'expression': e})[:400].replace('\n', ' '))
    call('close_document', {'path': name})
p.kill()

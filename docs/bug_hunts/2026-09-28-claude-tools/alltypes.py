import json, subprocess, os, select
H = os.path.dirname(os.path.abspath(__file__))
app = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=H + '/p/settings', QUCS_NO_SHELL_ENV='1', ASAN_OPTIONS='detect_leaks=0')
err = open(H + '/alltypes.err', 'w')
p = subprocess.Popen([app, '--mcp-server'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, env=env, text=True)
n = 0
def call(tool, args):
    global n; n += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": tool, "arguments": args}}) + '\n'); p.stdin.flush()
    while True:
        r, _, _ = select.select([p.stdout], [], [], 60)
        if not r: return 'HANG'
        line = p.stdout.readline()
        if not line: return 'EOF'
        m = json.loads(line)
        if m.get('id') == n: return m['result']
p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}) + '\n'); p.stdin.flush(); p.stdout.readline()
types = sorted({t['type'] for t in json.loads(call('list_component_types', {})['content'][0]['text'])})
bad = []
for sim in ('ngspice', 'xyce', 'qucsator'):
    call('set_simulator', {'simulator': sim})
    for t in types:
        r = call('describe_component_type', {'type': t})
        if r in ('HANG', 'EOF'): bad.append((sim, t, r)); print(sim, t, r); break
        r2 = call('add_component', {'type': t, 'x': 0, 'y': 0, 'preview': True})
        if r2 in ('HANG', 'EOF'): bad.append((sim, t, 'add ' + r2)); print(sim, t, r2); break
print(len(types), 'types x 3 simulators;', bad)

# For each example: its netlist; its JSON form (get_schematic json) set into a
# new document (set_schematic); that netlist. Lines compared (sorted, the
# title and the file names aside).
import json, subprocess, os, random, sys, select, re
H = os.path.dirname(os.path.abspath(__file__))
app = '/Users/meisam/git/Qucs-S_Enhancements/build/qucs/qucs-s.app/Contents/MacOS/qucs-s'
EX = os.environ.get('RT_EX', '/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/examples')
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=H + '/p/settings', QUCS_NO_SHELL_ENV='1')
os.makedirs(H + '/p/ws', exist_ok=True)   # (its workspace, not the user's)
p = subprocess.Popen([app, '--mcp-server', '--workspace', H + '/p/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
n = 0
def call(tool, args):
    global n; n += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": tool, "arguments": args}}) + '\n'); p.stdin.flush()
    while True:
        r, _, _ = select.select([p.stdout], [], [], 60)
        if not r: return None, 'HANG'
        m = json.loads(p.stdout.readline())
        if m.get('id') == n:
            res = m['result']; return res.get('isError'), ' '.join(c.get('text', '') for c in res['content'])
p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}) + '\n'); p.stdin.flush(); p.stdout.readline()
files = sorted(os.path.join(r, f) for r, _, fs in os.walk(EX + os.environ.get('RT_SUB', '/ngspice')) for f in fs if f.endswith('.sch') and not f.startswith('rt_copy_'))
random.Random(int(sys.argv[1])).shuffle(files)
def norm(t):
    out = []
    for l in t.split('\n'):
        l = l.strip()
        if not l or l.startswith('*') or l.startswith('The netlist of') or 'spice4qucs' in l or l.startswith('.INCLUDE'): continue
        out.append(re.sub(r'\s+', ' ', l))
    return sorted(out)
diffs = 0; done = 0
for f in files[:int(sys.argv[2])]:
    e, t = call('open_document', {'path': f})
    if e: continue
    e1, net1 = call('get_netlist', {})
    e2, js = call('get_schematic', {'format': 'json'})
    if e1 or e2: call('close_document', {'unsaved': 'discard'}); continue
    d = json.loads(js)
    call('new_document', {'kind': 'schematic'})
    # (Beside the original, when RT_BESIDE is set: its subcircuits and libraries found as they are there.)
    beside = os.path.join(os.path.dirname(f), 'rt_copy_' + os.path.basename(f)) if os.environ.get('RT_BESIDE') else None
    if beside: call('save_document', {'as': beside, 'replace': True})
    e3, t3 = call('set_schematic', {'components': d['components'], 'wires': d['wires']})
    if e3:
        print('SET FAILED', os.path.relpath(f, EX), t3[:300]); diffs += 1
    else:
        e4, net2 = call('get_netlist', {})
        a, b = norm(net1), norm(net2)
        if a != b:
            diffs += 1
            print('DIFF', os.path.relpath(f, EX)); print('   only before:', [x for x in a if x not in b][:4]); print('   only after: ', [x for x in b if x not in a][:4])
    done += 1
    call('close_document', {'unsaved': 'discard'}); call('close_document', {'unsaved': 'discard'})
    if beside and os.path.exists(beside): os.remove(beside)
print(done, 'round trips,', diffs, 'differ')

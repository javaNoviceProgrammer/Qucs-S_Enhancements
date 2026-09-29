# Drive `qucs-s --mcp-server`: calls from a JSON file [[tool, args], ...];
# prints each result's text (trimmed unless -v).
import json, subprocess, sys, os
S = os.path.dirname(os.path.abspath(__file__))
app = os.environ.get('QUCS', '/Users/meisam/git/Qucs-S_Enhancements/build/qucs/qucs-s.app/Contents/MacOS/qucs-s')
calls = json.load(open(sys.argv[1]))
full = '-v' in sys.argv
# Its settings and workspace: folders of its own (the workspace checked
# before the first call - the user's ~/QucsWorkspace is not for probes).
settings = os.environ.get('HUNT_SETTINGS', S + '/settings'); ws = os.environ.get('HUNT_WORKSPACE', S + '/ws')
os.makedirs(settings, exist_ok=True); os.makedirs(ws, exist_ok=True)
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=settings, QUCS_NO_SHELL_ENV='1')
p = subprocess.Popen([app, '--mcp-server', '--workspace', ws], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
def send(o):
    p.stdin.write(json.dumps(o) + '\n'); p.stdin.flush()
def recv(i):
    while True:
        line = p.stdout.readline()
        if not line: return None
        m = json.loads(line)
        if m.get('id') == i: return m
send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}); recv(0)
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": -1, "method": "tools/call", "params": {"name": "get_state", "arguments": {}}})
state = json.loads(recv(-1)['result']['content'][0]['text'])
if os.path.realpath(state['workspace']) != os.path.realpath(ws):
    p.kill(); sys.exit(f"the server works in {state['workspace']}, not {ws}: stopped before any call")
import time
for n, (tool, args) in enumerate(calls, 1):
    if tool == '_sh':
        print(f'--- {n} sh: {args}'); subprocess.run(args, shell=True); continue
    if tool == '_sleep':
        time.sleep(args); continue
    send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    m = recv(n)
    r = m.get('result', m)
    text = '\n'.join(c.get('text', '') for c in r.get('content', [])) if isinstance(r, dict) else str(r)
    err = ' ERROR' if isinstance(r, dict) and r.get('isError') else ''
    print(f'--- {n} {tool}{err}')
    print(text if full or len(text) < 1500 else text[:1500] + ' ...')
p.stdin.close(); p.wait(timeout=30)

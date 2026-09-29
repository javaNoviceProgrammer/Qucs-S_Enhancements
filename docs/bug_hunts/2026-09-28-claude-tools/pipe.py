# Sends JSON-RPC requests to qucs-s --mcp-server in groups without waiting
# (as an MCP client may), then reads every answer. Groups: [[[tool, args], ...], ...]
import json, subprocess, sys, os, time
H = os.path.dirname(os.path.abspath(__file__))
app = os.environ.get('QUCS', '/Users/meisam/git/Qucs-S_Enhancements/build/qucs/qucs-s.app/Contents/MacOS/qucs-s')
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=H + '/p/settings', QUCS_NO_SHELL_ENV='1')
os.makedirs(H + '/p/ws', exist_ok=True)   # (its workspace, not the user's)
p = subprocess.Popen([app, '--mcp-server', '--workspace', H + '/p/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)
n = 0
def send(o): p.stdin.write(json.dumps(o) + '\n'); p.stdin.flush()
def read(ids):
    got = {}
    while len(got) < len(ids):
        m = json.loads(p.stdout.readline())
        if m.get('id') in ids: got[m['id']] = m
    return got
send({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}}); read([0])
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
for group in json.load(open(sys.argv[1])):
    ids = []
    for tool, args in group:
        n += 1; ids.append(n)
        send({"jsonrpc": "2.0", "id": n, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    got = read(ids)
    for i, (tool, args) in zip(ids, group):
        r = got[i].get('result', got[i])
        t = ' '.join(c.get('text', '') for c in r.get('content', []))
        print(f'--- {i} {tool}{" ERROR" if r.get("isError") else ""}\n{t[:700]}')
p.stdin.close(); p.wait(timeout=30)

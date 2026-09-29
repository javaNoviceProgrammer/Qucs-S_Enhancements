import json, subprocess, os, select, time, sys
H = os.path.dirname(os.path.abspath(__file__))
app = os.environ.get('QUCS')
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=H + '/p/settings', QUCS_NO_SHELL_ENV='1', ASAN_OPTIONS='detect_leaks=0')
os.makedirs(H + '/p/ws', exist_ok=True)   # (its workspace, not the user's)
p = subprocess.Popen([app, '--mcp-server', '--workspace', H + '/p/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(H + '/rpc.err', 'w'), env=env)
def w(b): p.stdin.write(b); p.stdin.flush()
def drain(t=3):
    out = b''; end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.2)
        if r:
            c = os.read(p.stdout.fileno(), 1 << 20)
            if not c: return out + b'<EOF>'
            out += c
    return out
w(b'{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}\n'); drain(2)
cases = [
 b'{not json}\n',
 b'[{"jsonrpc":"2.0","id":1,"method":"tools/list"},{"jsonrpc":"2.0","id":2,"method":"ping"}]\n',
 b'{"jsonrpc":"2.0","id":null,"method":"ping"}\n',
 b'{"jsonrpc":"2.0","id":{"a":1},"method":"ping"}\n',
 b'{"jsonrpc":"2.0","id":"s","method":"tools/call","params":[1,2]}\n',
 b'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":42}}\n',
 b'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_state","arguments":"x"}}\n',
 b'{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"qucs://../../etc/passwd"}}\n',
 b'{"jsonrpc":"2.0","id":6,"method":"resources/subscribe","params":{}}\n',
 b'{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":99}}\n',
 b'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_state","arguments":{}}}\r\n',
 b'\n\n   \n',
 b'{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"set_schematic","arguments":{"text":"' + b'A' * 50_000_000 + b'"}}}\n',
 b'{"jsonrpc":"2.0","id":9,"method":"ping"}\n',
]
for c in cases:
    w(c); o = drain(6 if len(c) > 1000 else 2)
    print(repr(c[:90]), '->', o[:300])
    if o.endswith(b'<EOF>'): break
p.stdin.close(); p.wait(timeout=20)
print('exit', p.returncode)

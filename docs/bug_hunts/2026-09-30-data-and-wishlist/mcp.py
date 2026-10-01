import json, subprocess, os, sys, tempfile
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
APP = os.environ.get('QUCS', os.path.join(REPO, 'build/qucs/qucs-s.app/Contents/MacOS/qucs-s'))
# Workspaces, settings, a HOME and (for the fuzzers) exs/, a copy of the examples, go here.
HERE = os.environ.get('QUCS_PROBE_ROOT', os.path.join(tempfile.gettempdir(), 'qucs-probes'))
os.makedirs(HERE, exist_ok=True)
class Server:
    def __init__(self, ws):
        os.makedirs(ws, exist_ok=True); os.makedirs(HERE + '/settings', exist_ok=True); os.makedirs(HERE + '/home', exist_ok=True)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=HERE + '/settings', QUCS_NO_SHELL_ENV='1', HOME=HERE + '/home',
                   QUCS_LIBRARY_DIR=os.path.join(REPO, 'qucs-s-26.1.1/library'))
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', ws], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(HERE + '/server.err', 'a'), env=env, text=True)
        self.n = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
    def send(self, o):
        self.p.stdin.write(json.dumps(o) + '\n'); self.p.stdin.flush()
    def rpc(self, method, params):
        self.n += 1; i = self.n
        self.send({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params})
        while True:
            line = self.p.stdout.readline()
            if not line: raise RuntimeError('server died')
            m = json.loads(line)
            if m.get('id') == i: return m
    def call(self, tool, args, ok=True):
        m = self.rpc('tools/call', {'name': tool, 'arguments': args})
        r = m.get('result', m)
        text = '\n'.join(c.get('text', '') for c in r.get('content', [])) if isinstance(r, dict) else str(r)
        if isinstance(r, dict) and r.get('isError') and ok: raise RuntimeError(f'{tool} -> {text[:800]}')
        try: return json.loads(text)
        except Exception: return text
    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=30)

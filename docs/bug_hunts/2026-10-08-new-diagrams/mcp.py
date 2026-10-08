"""A Qucs-S --mcp-server of its own: workspace, settings, HOME, trash and cache in a scratch folder."""
import json, os, shutil, subprocess, time, itertools
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
APP = os.environ.get('QUCS', REPO + '/build/qucs/qucs-s.app/Contents/MacOS/qucs-s')
BASE = os.environ.get('HUNT_RUNS', '/tmp/qucs-hunt-runs')   # each server's workspace, settings, HOME, trash, cache
_count = itertools.count()
class Server:
    def __init__(self, name='s', settings_ini=None):
        self.root = f'{BASE}/{name}-{os.getpid()}-{next(_count)}'
        self.ws, self.settings, self.home = self.root + '/ws', self.root + '/settings', self.root + '/home'
        for d in (self.ws, self.settings + '/qucs', self.home, self.root + '/trash', self.root + '/cache'): os.makedirs(d, exist_ok=True)
        # Qucsator: the one built beside the app (an ini of the caller's replaces this one).
        open(self.settings + '/qucs/qucs_s.ini', 'w').write(settings_ini or f'[General]\nQucsator={REPO}/build/qucsator_rf/src/qucsator_rf\nOpenVAFExecutable={shutil.which("openvaf-r") or ""}\n')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=self.settings, QUCS_NO_SHELL_ENV='1', HOME=self.home,
                   QUCS_TRASH_DIR=self.root + '/trash', QUCS_CACHE_DIR=self.root + '/cache', QUCS_LIBRARY_DIR=REPO + '/qucs-s-26.1.1/library',
                   QUCSATOR=REPO + '/build/qucsator_rf/src/qucsator_rf', QUCSCONV=REPO + '/build/qucsator_rf/src/converter/qucsconv_rf')
        self.err = open(self.root + '/server.err', 'a')
        self.p = subprocess.Popen([APP, '--mcp-server', '--workspace', self.ws], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, env=env, text=True)
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
            if not line: raise RuntimeError('server died: ' + open(self.root + '/server.err').read()[-2000:])
            m = json.loads(line)
            if m.get('id') == i: return m
    def call(self, tool, args=None):
        t0 = time.time()
        m = self.rpc('tools/call', {'name': tool, 'arguments': args or {}})
        r = m.get('result', m)
        text = '\n'.join(c.get('text', '') for c in r.get('content', [])) if isinstance(r, dict) else str(r)
        self.last_time = time.time() - t0
        self.last_error = bool(isinstance(r, dict) and r.get('isError'))
        try: return json.loads(text)
        except Exception: return text
    def close(self):
        try:
            self.p.stdin.close(); self.p.wait(timeout=30)
        except Exception:
            self.p.kill()

"""A small client for qucs-s --mcp-server: isolated dirs, tools/call by name."""
import json
import os
import subprocess

APP = '/Users/meisam/git/Qucs-S_Enhancements/build/qucs/qucs-s.app/Contents/MacOS/qucs-s'
LIBRARY = '/Users/meisam/git/Qucs-S_Enhancements/qucs-s-26.1.1/library'


class Client:
    def __init__(self, work, app=APP, env_extra=None):
        for d in ('home', 'settings', 'cache', 'trash'):
            os.makedirs(os.path.join(work, d), exist_ok=True)
        env = dict(os.environ, HOME=work + '/home', QUCS_SETTINGS_DIR=work + '/settings', QUCS_CACHE_DIR=work + '/cache',
                   QUCS_TRASH_DIR=work + '/trash', QT_QPA_PLATFORM='offscreen', QUCS_LIBRARY_DIR=LIBRARY)
        env.update(env_extra or {})
        err = open(os.environ['MCP_STDERR'], 'w') if os.environ.get('MCP_STDERR') else subprocess.DEVNULL
        self.p = subprocess.Popen([app, '--mcp-server'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=err, env=env, text=True)
        self.n = 0
        self._send({'jsonrpc': '2.0', 'id': 0, 'method': 'initialize',
                    'params': {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'probe', 'version': '1'}}})
        self._answer(0)
        self._send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def _send(self, m):
        self.p.stdin.write(json.dumps(m) + '\n')
        self.p.stdin.flush()

    def _answer(self, i):
        while True:
            line = self.p.stdout.readline()
            if not line:
                return None
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if m.get('id') == i:
                return m

    def call(self, name, args=None):
        self.n += 1
        self._send({'jsonrpc': '2.0', 'id': self.n, 'method': 'tools/call', 'params': {'name': name, 'arguments': args or {}}})
        m = self._answer(self.n) or {}
        r = m.get('result', {})
        text = ''.join(c.get('text', '') for c in r.get('content', []) if c.get('type') == 'text')
        return bool(r.get('isError')), text

    def json(self, name, args=None):
        err, text = self.call(name, args)
        try:
            return err, json.loads(text)
        except ValueError:
            return err, text

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=60)
        except Exception:
            self.p.kill()

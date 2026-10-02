import json, os, subprocess
class Server:
    def __init__(self, root, repo, app):
        for d in ('ws', 'settings', 'home', 'trash'): os.makedirs(root + '/' + d, exist_ok=True)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=root + '/settings', QUCS_NO_SHELL_ENV='1', HOME=root + '/home',
                   QUCS_TRASH_DIR=root + '/trash', QUCS_LIBRARY_DIR=repo + '/qucs-s-26.1.1/library')
        self.p = subprocess.Popen([app, '--mcp-server', '--workspace', root + '/ws'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(root + '/server.err', 'w'), env=env, text=True)
        self.n = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'repro', 'version': '1'}})
        self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n'); self.p.stdin.flush()
    def rpc(self, method, params):
        self.n += 1; i = self.n
        self.p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params}) + '\n'); self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line: raise RuntimeError('server died')
            m = json.loads(line)
            if m.get('id') == i: return m
    def call(self, tool, args):
        r = self.rpc('tools/call', {'name': tool, 'arguments': args})['result']
        return r.get('isError', False), [c.get('text', '[image]') for c in r['content']]
    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=30)

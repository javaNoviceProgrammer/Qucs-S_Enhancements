# A client of `qucs-s --mcp-server` for the hunt's fuzzers (from the hunt of
# 16 October's fuzz_tools.py): a server in a folder of its own (workspace,
# settings, HOME, cache), offscreen, its stderr to a file; rpc() returns the
# answer, 'EOF' (it died) or 'HANG' (no answer in time).
import json, os, select, subprocess, sys, time


class Server:
    def __init__(self, app, home, errpath, extra_env=None):
        self.home = home
        self.ws = os.path.join(home, 'w', 'ws')
        os.makedirs(self.ws, exist_ok=True)
        os.makedirs(os.path.join(home, 'home'), exist_ok=True)
        self.errpath = errpath
        self.err = open(errpath, 'ab')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=os.path.join(home, 'settings'),
                   HOME=os.path.join(home, 'home'), QUCS_NO_SHELL_ENV='1', QUCS_CLAUDE='/nonexistent/claude',
                   ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='print_stacktrace=1')
        env.update(extra_env or {})
        self.p = subprocess.Popen([app, '--mcp-server', '--workspace', self.ws], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.err, env=env, bufsize=0, start_new_session=True)
        self.n = 0
        self.buf = b''
        r = self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}}, timeout=180)
        if not isinstance(r, dict):
            raise RuntimeError(f'the server did not start: {r}')
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, o):
        try:
            self.p.stdin.write((json.dumps(o) + '\n').encode())
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def line(self, end):
        while b'\n' not in self.buf:
            left = end - time.time()
            if left <= 0: return 'HANG'
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r: return 'HANG'
            chunk = os.read(self.p.stdout.fileno(), 1 << 20)
            if not chunk: return 'EOF'
            self.buf += chunk
        l, _, self.buf = self.buf.partition(b'\n')
        return l.decode('utf-8', 'replace')

    def rpc(self, method, params, timeout=90):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params})
        end = time.time() + timeout
        while True:
            line = self.line(end)
            if line in ('HANG', 'EOF'): return line
            try: m = json.loads(line)
            except Exception: continue
            if m.get('id') == self.n: return m
            # (a line it could not read - nested past Qt's 1024 levels -: its parse error, id null)
            if m.get('id') is None and 'error' in m: return m

    def call(self, name, args=None, timeout=90):
        return self.rpc('tools/call', {'name': name, 'arguments': args or {}}, timeout=timeout)

    def text(self, r):
        if not isinstance(r, dict) or 'result' not in r: return str(r)
        return ' '.join(c.get('text', '') for c in r['result'].get('content', []))

    def kill(self):
        try:
            self.p.kill(); self.p.wait(10)
        except Exception:
            pass


def sanitizer_lines(errpath, pos):
    with open(errpath, 'rb') as f:
        f.seek(pos); new = f.read().decode('utf-8', 'replace'); end = f.tell()
    hits = [l for l in new.splitlines() if 'runtime error' in l or 'AddressSanitizer' in l or 'ERROR:' in l
            or 'SUMMARY' in l or 'ASSERT' in l or 'Assertion' in l]
    return hits, end, new

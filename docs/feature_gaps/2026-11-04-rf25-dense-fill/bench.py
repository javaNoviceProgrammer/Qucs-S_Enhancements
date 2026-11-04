#!/usr/bin/env python3
"""bench.py APP LABEL MODE REPS [OUT.jsonl]

rf25_solve on fresh copies of the fgc S62 model (its .qrf25 and its .gds,
never its .solution), REPS solves in one headless app process: MODE "one"
is 10 GHz alone, "sweep" the model's own 40 frequencies. Each solve gets a
folder of its own under BENCH_RUNS (a folder in the system's temporary
one when not given), with the app's home, settings, cache and trash beside
it; nothing is written to ~/QucsWorkspace or the user's caches. One JSON
line per solve to OUT (BENCH_RUNS/results.jsonl): the wall time, the
matrix answer (fill s, LU s, LU by), threads, the unknowns, S at the
frequency, the sweep's dB, and the kept .solution's path, copied out.
"""
import json, os, shutil, subprocess, sys, tempfile, time

HERE = os.environ.get('BENCH_RUNS', os.path.join(tempfile.gettempdir(), 'rf25-dense-fill-bench'))
SRC = os.path.expanduser('~/QucsWorkspace/bench_ponchak_fgc/sims')
# (BENCH_MODEL and BENCH_GDS name another of the folder's models.)
MODEL = os.environ.get('BENCH_MODEL', 'fgc_S62_L1000_r15p6_c10.qrf25')
GDS = os.environ.get('BENCH_GDS', 'fgc_S62_L1000.gds')


class Server:
    def __init__(self, app, root):
        self.root = root
        d = {k: os.path.join(root, k) for k in ('ws', 'settings', 'home', 'trash', 'cache')}
        for v in d.values():
            os.makedirs(v, exist_ok=True)
        self.ws = d['ws']
        # (BENCH_INI: lines of the app's settings, written before it starts -
        #  "[RF25]\ndenseLimit=8000" for RF 2.5D > Settings...'s.)
        if os.environ.get('BENCH_INI'):
            os.makedirs(os.path.join(d['settings'], 'qucs'), exist_ok=True)
            with open(os.path.join(d['settings'], 'qucs', 'qucs_s.ini'), 'w') as f:
                f.write(os.environ['BENCH_INI'].replace('\\n', '\n') + '\n')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=d['settings'], QUCS_NO_SHELL_ENV='1',
                   HOME=d['home'], QUCS_TRASH_DIR=d['trash'], QUCS_CACHE_DIR=d['cache'],
                   QUCS_CLAUDE='/nonexistent/claude', QUCS_GH='/nonexistent/gh')
        for k in ('QUCS_RF25_LU', 'QUCS_FEM_SOLVER'):
            env.pop(k, None)
        self.p = subprocess.Popen([app, '--mcp-server', '--workspace', self.ws], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=open(os.path.join(root, 'server.err'), 'a'),
                                  env=env, text=True)
        self.n = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, o):
        self.p.stdin.write(json.dumps(o) + '\n')
        self.p.stdin.flush()

    def rpc(self, method, params):
        self.n += 1
        i = self.n
        self.send({'jsonrpc': '2.0', 'id': i, 'method': method, 'params': params})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('server died')
            m = json.loads(line)
            if m.get('id') == i:
                return m

    def call(self, tool, args):
        r = self.rpc('tools/call', {'name': tool, 'arguments': args}).get('result', {})
        text = '\n'.join(c.get('text', '') for c in r.get('content', []))
        if r.get('isError'):
            raise RuntimeError('%s: %s' % (tool, text[:800]))
        try:
            return json.loads(text)
        except ValueError:
            return text

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)


def main():
    app, label, mode, reps = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    out = sys.argv[5] if len(sys.argv) > 5 else os.path.join(HERE, 'results.jsonl')
    root = os.path.join(HERE, 'runs', '%s-%s-%s-%d' % (time.strftime('%H%M%S'), label, mode, os.getpid()))
    s = Server(app, root)
    try:
        for r in range(reps):
            d = os.path.join(s.ws, 'r%d' % r)
            os.makedirs(d)
            for f in (MODEL, GDS):
                shutil.copy2(os.path.join(SRC, f), d)
            path = os.path.join(d, MODEL)
            args = {'path': path, 'solver': 'full_wave', 'matrix': 'dense', 'timeout': 3600}
            if mode == 'one':
                args['frequency'] = '10[GHz]'
            t0 = time.time()
            a = s.call('rf25_solve', args)
            wall = time.time() - t0
            kept = path + '.solution'
            saved = None
            if os.path.exists(kept):
                saved = os.path.join(root, 'r%d.solution' % r)
                shutil.copy2(kept, saved)
            row = {'label': label, 'model': MODEL, 'mode': mode, 'rep': r, 'wall': round(wall, 3), 'matrix': a.get('matrix'),
                   'threads': a.get('threads'), 'unknowns': a.get('unknowns'), 's': a.get('s matrix'),
                   'z': a.get('z matrix'), 'sweep': a.get('sweep'), 'checks': a.get('checks'), 'solution': saved,
                   'when': time.strftime('%H:%M:%S'), 'load': os.getloadavg()[0]}
            with open(out, 'a') as f:
                f.write(json.dumps(row) + '\n')
            m = a.get('matrix', {})
            print('%s %s %d: wall %.2f s, fill %s, LU %s (%s), threads %s, unknowns %s, load %.1f'
                  % (label, mode, r, wall, m.get('fill s'), m.get('LU s'), m.get('LU by'), a.get('threads'),
                     a.get('unknowns'), os.getloadavg()[0]), flush=True)
            s.call('close_document', {'path': path, 'unsaved': 'discard'})
    finally:
        s.close()


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# The optimization of a real model, through `qucs-s --mcp-server`, as Claude
# drives it: the shipped microstrip's width for 50 ohm by each of three methods,
# the coax's radius ratio against its closed form, a heater's two objectives.
# Its output is what the write-up quotes.
#
#   QUCS=<qucs-s> python3 drive.py <scratch folder>
import json, math, os, select, shutil, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
EX = os.path.join(ROOT, 'qucs-s-26.1.1', 'examples', 'multiphysics')
RUN = os.path.abspath(sys.argv[1])
WS = os.path.join(RUN, 'ws')
for d in ('ws', 'settings', 'home', 'cache', 'trash'): os.makedirs(os.path.join(RUN, d), exist_ok=True)


class Server:
    def __init__(self):
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QUCS_SETTINGS_DIR=os.path.join(RUN, 'settings'), HOME=os.path.join(RUN, 'home'),
                   QUCS_CACHE_DIR=os.path.join(RUN, 'cache'), QUCS_TRASH_DIR=os.path.join(RUN, 'trash'), QUCS_NO_SHELL_ENV='1')
        self.p = subprocess.Popen([os.environ['QUCS'], '--mcp-server', '--workspace', WS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=open(os.path.join(RUN, 'stderr.txt'), 'w'), env=env, bufsize=0)
        self.n = 0
        self.buf = b''
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}})
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, o):
        self.p.stdin.write((json.dumps(o) + '\n').encode())
        self.p.stdin.flush()

    def rpc(self, method, params, timeout=900):
        self.n += 1
        self.send({'jsonrpc': '2.0', 'id': self.n, 'method': method, 'params': params})
        end = time.time() + timeout
        while True:
            while b'\n' not in self.buf:
                r, _, _ = select.select([self.p.stdout], [], [], max(0.1, end - time.time()))
                if not r:
                    raise TimeoutError(method)
                chunk = os.read(self.p.stdout.fileno(), 1 << 20)
                if not chunk:
                    raise EOFError(method)
                self.buf += chunk
            line, _, self.buf = self.buf.partition(b'\n')
            try:
                m = json.loads(line)
            except Exception:
                continue
            if m.get('id') == self.n:
                return m

    def call(self, name, **args):
        t0 = time.time()
        r = self.rpc('tools/call', {'name': name, 'arguments': args}).get('result', {})
        text = ''.join(c.get('text', '') for c in r.get('content', []))
        try:
            data = json.loads(text)
        except Exception:
            data = text
        return data, r.get('isError', False), time.time() - t0


s = Server()


def copy(name):
    to = os.path.join(WS, name)
    shutil.copy(os.path.join(EX, name), to)
    os.chmod(to, 0o644)
    return to


def show(title, r):
    data, bad, took = r
    if bad:
        print(f'{title}: REFUSED ({took:.1f} s): {data}')
        return data
    best = ', '.join(f"{v['name']} = {v.get('value', v.get('text'))}" for v in data.get('best', []))
    there = ', '.join(f"{v['name']} = {v.get('value', v.get('text'))}" for v in data.get('objectives there', []))
    print(f"{title}: {data['status']}, {data['evaluations']} evaluations, {data['solves']} solves, {data['failed']} failed, {data['seconds']:.2f} s")
    print(f'  best: {best}\n  there: {there}')
    for note in data.get('notes', []): print(f'  note: {note}')
    for line in data.get('limits', []): print(f'  limit: {line}')
    return data


# A microstrip's width for 50 ohm: a real model, whose mesh changes with the width.
path = copy('microstrip.qfem')
print(s.call('open_document', path=path)[1])
print('parameters:', [(p['name'], p.get('value')) for p in s.call('fem_describe', path=path, what='model')[0]['parameters']])
for method, extra in (('tune', {'maxiter': 12, 'tol': '1e-4'}), ('lm', {'maxiter': 10, 'tol': '1e-5', 'fdstep': '0.02'}), ('tr', {'maxiter': 25, 'tol': '1e-5'}), ('bayes', {'maxiter': 20, 'seed': 1})):
    props = dict({'method': method, 'variables': [['w', '', '1[mm]', '6[mm]']], 'objectives': [['lp1.Z0', 'TARGET', '50[ohm]']]}, **extra)
    a = s.call('fem_edit', action='add', path=path, parent='model', type='optimization', props=props, label=f'50 ohm by {method}')
    if a[1]:
        print('add refused:', a[0])
        continue
    tag = a[0]['added']['tag']
    show(f'microstrip w for 50 ohm, {method} ({tag})', s.call('fem_optimize', path=path, optimization=tag, history=0, timeout=600))
# Written in, solved there.
print('apply:', s.call('fem_optimize', path=path, optimization='opt1', action='apply')[0])
solved = s.call('fem_solve', path=path, timeout=300)[0]
print('solved there:', [d for d in solved.get('derived values', solved.get('derived', [{}]))][:1])
print(s.call('close_document', path=path, unsaved='discard')[1])

# The coax: es.C11 = 2 pi eps0 er / ln(b/a), a closed form on a mesh that cannot
# be exact (a circle's arcs): the ratio for 100 pF per metre at er = 2.1.
path = copy('coax.qfem')
print(s.call('open_document', path=path)[1])
exact = 1.5e-3 / math.exp(2 * math.pi * 8.8541878188e-12 * 2.1 / 100e-12)
for method, extra in (('tune', {'maxiter': 12, 'tol': '1e-4'}), ('cmaes', {'maxiter': 12, 'seed': 1, 'polish': True}), ('nm', {'maxiter': 40, 'tol': '1e-4'})):
    props = dict({'method': method, 'variables': [['a', '', '0.2[mm]', '1.2[mm]']], 'objectives': [['es.C11', 'TARGET', '100[pF]']]}, **extra)
    a = s.call('fem_edit', action='add', path=path, parent='model', type='optimization', props=props, label=f'100 pF by {method}')
    tag = a[0]['added']['tag']
    data = show(f'coax a for 100 pF, {method} (closed form {exact * 1e3:.5f} mm)', s.call('fem_optimize', path=path, optimization=tag, history=0, timeout=600))
s.call('close_document', path=path, unsaved='discard')

# Two objectives of a real device: a resistor film's power against its peak
# temperature, as a Pareto front.
path = copy('resistor_heating.qfem')
print(s.call('open_document', path=path)[1])
props = {'method': 'nsga2', 'maxiter': 6, 'population': 12, 'seed': 1, 'variables': [['V0', '', '0.1[V]', '1[V]'], ['L', '', '1[mm]', '4[mm]']], 'objectives': [['ec.P', 'MAX'], ['ht.Tmax', 'MIN']]}
a = s.call('fem_edit', action='add', path=path, parent='model', type='optimization', props=props, label='Power against temperature')
r = s.call('fem_optimize', path=path, history=0, timeout=600)
data = r[0]
if r[1]:
    print('resistor: REFUSED', data)
else:
    print(f"resistor Pareto front: {data['status']}, {data['evaluations']} evaluations, {data['solves']} solves, {data['failed']} failed, {data['seconds']:.2f} s")
    for d in data.get('Pareto front', []):
        print('  ', d['point'], {k: float(f'{v:.4g}') for k, v in d['values'].items()}, {k: float(f'{v:.4g}') for k, v in d['objectives'].items()})
s.call('close_document', path=path, unsaved='discard')
s.p.kill()

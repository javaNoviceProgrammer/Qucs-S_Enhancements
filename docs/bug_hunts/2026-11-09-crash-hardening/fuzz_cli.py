# A command-line program fed mutated input files, in parallel: a crash (a
# signal), a sanitizer's report on stderr or a hang (no exit in time) is
# kept with the input that made it.
#   python3 fuzz_cli.py --corpus DIR --ext .net --out DIR --seconds N --jobs J -- PROG ARGS...
# In ARGS, {in} is the mutated file's path, {out} a file for the program to
# write and {dir} the case's folder. Files in the corpus beside the inputs
# (Touchstone files a netlist names, say) are copied into each case's
# folder; with --mutate-aux they are mutated too.
import argparse, os, random, shutil, signal, subprocess, sys, time, hashlib
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mutate import mutate

ap = argparse.ArgumentParser()
ap.add_argument('--corpus', required=True)
ap.add_argument('--ext', required=True, help='comma-separated suffixes of the inputs')
ap.add_argument('--out', required=True)
ap.add_argument('--seconds', type=int, default=600)
ap.add_argument('--jobs', type=int, default=6)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--timeout', type=float, default=20)
ap.add_argument('--mutate-aux', action='store_true')
ap.add_argument('--xml', action='store_true')
ap.add_argument('--stdin', action='store_true', help='also give the input on stdin')
ap.add_argument('cmd', nargs=argparse.REMAINDER)
a = ap.parse_args()
cmd = a.cmd[1:] if a.cmd and a.cmd[0] == '--' else a.cmd
exts = tuple(a.ext.split(','))

inputs, aux = [], []
for root, _, fs in os.walk(a.corpus):
    for f in fs:
        p = os.path.join(root, f)
        (inputs if f.endswith(exts) else aux).append(p)
inputs.sort(); aux.sort()
if not inputs: sys.exit('no inputs')
os.makedirs(a.out, exist_ok=True)
crashes = os.path.join(a.out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(a.out, 'log.txt'), 'a')
seen = {}
stats = {'runs': 0, 'crash': 0, 'hang': 0, 'san': 0, 'rc': {}}
end = time.time() + a.seconds
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0:symbolize=1', UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=0',
           QT_QPA_PLATFORM='offscreen')


def signature(err, rc):
    # the first frames of the report in the program's own code, or the signal
    lines = [l for l in err.splitlines() if (' in ' in l and '#' in l) or 'runtime error' in l or 'ERROR: AddressSanitizer' in l]
    key = '\n'.join(l.split(' (')[0][-120:] for l in lines[:4]) or f'rc={rc}'
    return hashlib.sha1(key.encode()).hexdigest()[:10], key


def one(k):
    r = random.Random(a.seed * 1000003 + k)
    src = r.choice(inputs)
    d = os.path.join(a.out, 'w', str(k % (a.jobs * 4)))
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    for x in aux:
        data = open(x, 'rb').read()
        if a.mutate_aux and r.random() < 0.5: data = mutate(data, r)
        open(os.path.join(d, os.path.basename(x)), 'wb').write(data)
    data = mutate(open(src, 'rb').read(), r, xml=a.xml)
    inp = os.path.join(d, 'in' + os.path.splitext(src)[1])
    open(inp, 'wb').write(data)
    args = [c.replace('{in}', inp).replace('{out}', os.path.join(d, 'out')).replace('{dir}', d) for c in cmd]
    t0 = time.time()
    try:
        p = subprocess.run(args, cwd=d, env=env, stdin=open(inp, 'rb') if a.stdin else subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=a.timeout, start_new_session=True)
        rc, err = p.returncode, p.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        rc, err = 'HANG', (e.stderr or b'').decode('utf-8', 'replace')
    kind = None
    if rc == 'HANG': kind = 'hang'
    elif 'ERROR: AddressSanitizer' in err or 'runtime error:' in err: kind = 'san'
    elif isinstance(rc, int) and rc < 0 and rc not in (-signal.SIGTERM, -signal.SIGKILL): kind = 'crash'
    return k, src, data, rc, err, kind, time.time() - t0, d


def record(res):
    k, src, data, rc, err, kind, dt, d = res
    stats['runs'] += 1
    stats['rc'][str(rc)] = stats['rc'].get(str(rc), 0) + 1
    if not kind: return
    stats[kind] += 1
    sig, key = signature(err, rc)
    first = sig not in seen
    seen.setdefault(sig, 0); seen[sig] += 1
    if first:
        base = os.path.join(crashes, f'{kind}-{sig}')
        open(base + os.path.splitext(src)[1], 'wb').write(data)
        for x in os.listdir(d):
            if x not in ('out',) and not x.startswith('in.'): shutil.copy(os.path.join(d, x), base + '.aux.' + x)
        open(base + '.txt', 'w').write(f'case {k} from {src} rc={rc} {dt:.1f}s\n{key}\n\n{err[-20000:]}')
        print(f'NEW {kind} {sig} rc={rc} from {os.path.basename(src)} (case {k})', flush=True)
        log.write(f'NEW {kind} {sig} rc={rc} case {k} {src}\n'); log.flush()


k = 0
with ThreadPoolExecutor(a.jobs) as ex:
    futs = set()
    while time.time() < end:
        while len(futs) < a.jobs * 2:
            futs.add(ex.submit(one, k)); k += 1
        done = [f for f in futs if f.done()]
        for f in done:
            futs.discard(f); record(f.result())
        if not done: time.sleep(0.02)
    for f in futs: record(f.result())
summary = f"{stats['runs']} runs, {stats['crash']} crashes, {stats['san']} sanitizer reports, {stats['hang']} hangs, {len(seen)} distinct; rc {stats['rc']}"
print(summary); log.write(summary + '\n')

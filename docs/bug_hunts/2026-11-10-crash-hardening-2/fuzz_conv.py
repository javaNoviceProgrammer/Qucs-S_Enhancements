# qucsconv_rf, the converter the program runs to import data and SPICE
# netlists, on damaged files of each of its input formats (mutate.py), into
# each output format it pairs them with. A signal, a hang past TIMEOUT or a
# sanitizer's report is kept with the file and the command.
#   QUCSCONV=<qucsconv_rf> python3 fuzz_conv.py SEEDS OUT SEED SECONDS
import os, random, shutil, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '2026-11-09-crash-hardening'))
from mutate import mutate

CONV = os.environ['QUCSCONV']
TIMEOUT = float(os.environ.get('TIMEOUT', '20'))
seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rnd = random.Random(seed)
work = os.path.join(out, f'run-{seed}'); shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
FORMATS = {'.zvr': ('zvr', ['qucsdata']), '.citi': ('citi', ['qucsdata']), '.csv': ('csv', ['qucsdata']),
           '.vcd': ('vcd', ['qucsdata']), '.mdl': ('mdl', ['qucsdata']), '.dat': ('qucsdata', ['csv', 'touchstone', 'matlab']),
           '.cir': ('spice', ['qucs', 'qucslib'])}
seeds = sorted(os.path.join(seeds_dir, f) for f in os.listdir(seeds_dir))
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0', UBSAN_OPTIONS='print_stacktrace=1')
t0 = time.time(); k = 0; events = 0
while time.time() - t0 < seconds:
    k += 1
    src = rnd.choice(seeds)
    ext = os.path.splitext(src)[1]
    fmt, outs = FORMATS.get(ext, ('touchstone', ['qucsdata']))
    data = open(src, 'rb').read()
    for _ in range(rnd.choice([1, 1, 2, 4])): data = mutate(data, rnd)
    inp = os.path.join(work, 'in' + ext)
    open(inp, 'wb').write(data)
    cmd = [CONV, '-if', fmt, '-of', rnd.choice(outs), '-i', inp, '-o', os.path.join(work, 'out')]
    if rnd.random() < 0.2: cmd += rnd.choice([['-a'], ['-c'], ['-g', 'gnd'], ['-d', 'S'], ['-ln', 'L']])
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT, env=env, stdin=subprocess.DEVNULL)
        rc, err = p.returncode, p.stderr.decode('utf-8', 'replace')
        what = (f'SIG{-rc}' if rc < 0 else 'SANITIZER' if ('runtime error' in err or 'AddressSanitizer' in err) else None)
    except subprocess.TimeoutExpired:
        what, err = 'HANG', ''
    if what:
        events += 1
        c = os.path.join(crashes, f'{what.lower()}-{seed}-{k}')
        os.makedirs(c, exist_ok=True)
        shutil.copy(inp, c)
        open(os.path.join(c, 'cmd.txt'), 'w').write(' '.join(cmd[1:]) + '\n' + src + '\n')
        open(os.path.join(c, 'stderr.txt'), 'w').write(err[-100000:])
        print(f'[{k}] {what} {os.path.basename(src)} {cmd[1:5]}', flush=True)
print(f'done: {k} runs, {events} events in {int(time.time() - t0)} s', flush=True)

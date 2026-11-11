# The program started on damaged settings: qucs_s.ini written with hostile
# values for the keys the sources read (KEYS: one per line, `Group/key` or a
# key of [General]) - numbers past range, NaN, fonts and colours that are
# not, byte arrays of noise for saved geometry and state, long strings and
# lists -, then a schematic and a data display opened, drawn, simulated and
# closed. A start that never answers, a crash (EOF), a hang or a sanitizer's
# report is kept with the settings.
#   QUCS=<qucs-s> python3 fuzz_settings.py KEYS SEEDS OUT SEED SECONDS
import os, random, shutil, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '2026-11-09-crash-hardening'))
from mcp import Server, sanitizer_lines

APP = os.environ['QUCS']
keys_file, seeds_dir, out, seed, seconds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
rnd = random.Random(seed)
keys = [k.strip() for k in open(keys_file) if k.strip() and ' ' not in k.strip()]
crashes = os.path.join(out, 'crashes'); os.makedirs(crashes, exist_ok=True)
log = open(os.path.join(out, f'log-{seed}.txt'), 'w')
schs = sorted(os.path.join(seeds_dir, f) for f in os.listdir(seeds_dir) if f.endswith('.sch'))
dpls = sorted(os.path.join(seeds_dir, f) for f in os.listdir(seeds_dir) if f.endswith('.dpl'))


def noise(n):
    return ''.join('\\x%02x' % rnd.randrange(256) for _ in range(n))


def value(key):
    k = key.lower()
    r = rnd.random()
    if 'font' in k and r < 0.7:
        return rnd.choice(['', 'x', ',,,,,,,,,,', 'Helvetica,-1,-1,5,50,0,0,0,0,0', 'A,1e308,0,0,0', 'B,2147483647,99999,99,9999,1,1,1,1,1',
                           'C,-2147483648', ','.join(['9'] * 60), 'D,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'])
    if ('colo' in k or 'pen' in k) and r < 0.7:
        return rnd.choice(['#zzzzzz', '#', '', '#ff', '#1234567890', 'transparent', 'nan', '@Variant(\\0\\0\\0\\x43\\x1)'])
    if any(w in k for w in ('geometry', 'state', 'splitter', 'header', 'dock')) and r < 0.7:
        return '@ByteArray(' + noise(rnd.choice([0, 1, 4, 16, 64, 400])) + ')'
    if any(w in k for w in ('size', 'width', 'height', 'count', 'max', 'num', 'zoom', 'scale', 'grid', 'interval', 'limit', 'depth', 'timeout', 'x', 'y')) and r < 0.7:
        return rnd.choice(['-1', '0', '2147483647', '-2147483648', '2147483648', '1e308', '-1e308', 'nan', 'inf', '1e-308', '99999999999999999999', ''])
    r = rnd.random()
    if r < 0.15: return 'x' * rnd.choice([1000, 100000])
    if r < 0.3: return ', '.join(['/nonexistent/' + str(i) for i in range(rnd.choice([3, 1000, 20000]))])
    if r < 0.4: return '@ByteArray(' + noise(rnd.choice([1, 16, 300])) + ')'
    if r < 0.5: return rnd.choice(['@Rect(0 0 -1 -1)', '@Size(-2147483648 -2147483648)', '@Point(2147483647 2147483647)', '@Rect(1e308 0 0 0)'])
    if r < 0.6: return rnd.choice(['true', 'false', 'maybe', '2', '-1'])
    if r < 0.75: return rnd.choice(['/', '..', '/dev/null', '/nonexistent', '~', '$HOME', '"', '\\', '\\0'])
    return rnd.choice(['-1', '0', '2147483648', '1e308', 'nan', '', ' '])


def write_settings(home):
    d = os.path.join(home, 'settings', 'qucs'); os.makedirs(d, exist_ok=True)
    chosen = rnd.sample(keys, min(len(keys), rnd.choice([5, 20, 60, len(keys)])))
    groups = {}
    for k in chosen:
        g, _, name = k.rpartition('/')
        groups.setdefault(g or 'General', []).append((name, value(k)))
    with open(os.path.join(d, 'qucs_s.ini'), 'w') as f:
        for g, items in groups.items():
            f.write(f'[{g}]\n')
            for name, v in items:
                f.write(f'{name}={v}\n')
            f.write('\n')
    return os.path.join(d, 'qucs_s.ini')


t0 = time.time(); k = 0; events = 0
while time.time() - t0 < seconds:
    k += 1
    home = os.path.join(out, f'run-{seed}')
    shutil.rmtree(home, ignore_errors=True); os.makedirs(home)
    ini = write_settings(home)
    errpath = os.path.join(home, 'stderr.txt')
    what, done = None, []
    try:
        s = Server(APP, home, errpath)
    except RuntimeError as e:
        what, s = 'START', None
        done.append(('initialize', str(e)))
    if s:
        sch = os.path.join(s.ws, 'a.sch'); shutil.copy(rnd.choice(schs), sch)
        calls = [('open_document', {'path': sch}, 90), ('get_schematic', {'path': sch, 'format': 'overview'}, 60),
                 ('export_image', {'path': sch, 'save_as': 'a.png'}, 90),
                 ('simulate', {'path': sch, 'timeout': 30, 'brief': True}, 120),
                 ('show_data_display', {'path': sch}, 60), ('screenshot', {}, 60),
                 ('close_document', {'path': sch, 'unsaved': 'discard'}, 60)]
        if dpls:
            dpl = os.path.join(s.ws, 'b.dpl'); shutil.copy(rnd.choice(dpls), dpl)
            calls.insert(5, ('open_document', {'path': dpl}, 60))
        for tool, args, timeout in calls:
            r = s.call(tool, args, timeout)
            done.append((tool, s.text(r)[:200] if isinstance(r, dict) else r))
            if r in ('HANG', 'EOF'): what = r; break
        s.kill()
    hits, _, new = sanitizer_lines(errpath, 0) if os.path.exists(errpath) else ([], 0, '')
    if hits and not what: what = 'SANITIZER'
    if what:
        events += 1
        c = os.path.join(crashes, f'{what.lower()}-{seed}-{k}'); os.makedirs(c, exist_ok=True)
        shutil.copy(ini, c)
        open(os.path.join(c, 'calls.txt'), 'w').write('\n'.join(map(str, done)) + '\n')
        open(os.path.join(c, 'stderr.txt'), 'w').write(new[-200000:])
        print(f'[{k}] {what} at {done[-1][0]}', file=log, flush=True)
    elif k % 10 == 0:
        print(f'[{k}] {int(time.time() - t0)} s, {events} events', file=log, flush=True)
print(f'done: {k} starts, {events} events', file=log, flush=True)

# The --json mode of the bundled programs (qucs-sfilter, qucs-sactivefilter,
# qucs-sattenuator, qucs-spowercombining, qucs-strans, rxcalc): specs made
# from the schema of the tool for Claude that runs each (synthesize_filter,
# synthesize_attenuator, synthesize_power_combiner, line_calc,
# receiver_budget) and the keys the program reads, with hostile values -
# wrong types, huge, NaN, negative, deep, long - and now and then bytes that
# are no JSON. A signal, a sanitizer's report or no answer in time is kept
# with the spec.
#   python3 fuzz_tooljson.py BIN_DIR TOOLS_JSON OUT SEED SECONDS JOBS
import json, os, random, signal, subprocess, sys, time, hashlib
from concurrent.futures import ThreadPoolExecutor
sys.setrecursionlimit(100000)

bindir, tools_json, out, seed, seconds, jobs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
schemas = {t['name']: t['inputSchema'] for t in json.load(open(tools_json))}
PROGS = {
    'qucs-sfilter': ('synthesize_filter', ['type', 'class', 'realization', 'fc', 'f2', 'fs', 'impedance', 'ripple', 'atten', 'order', 'substrate', 'simulator']),
    'qucs-sactivefilter': ('synthesize_filter', ['type', 'class', 'realization', 'fc', 'f2', 'fs', 'ripple', 'atten', 'order', 'gain', 'ap', 'transition', 'simulator']),
    'qucs-sattenuator': ('synthesize_attenuator', ['attenuation', 'f', 'lumped', 'p_in', 'r_below_z0', 's_parameters', 'topology', 'z_in', 'z_out', 'simulator']),
    'qucs-spowercombining': ('synthesize_power_combiner', ['alpha', 'f', 'implementation', 'ratio_db', 's_parameters', 'stages', 'substrate', 'type', 'ways', 'z0', 'simulator']),
    'qucs-strans': ('line_calc', ['do', 'solve_for', 'type', 'z0', 'z0e', 'dx', 'dy']),
    'rxcalc': ('receiver_budget', ['bandwidth', 'stages', 'temperature', 'input_power', 'snr_min', 'peak_to_average']),
}
paths = {}
for p in PROGS:
    for root, _, fs in os.walk(bindir):
        if p in fs and '/Contents/MacOS' in root: paths[p] = os.path.join(root, p)
os.makedirs(out, exist_ok=True)
rnd0 = random.Random(seed)

WS = ['', ' ', 'x', 'NaN', 'nan', 'inf', '-inf', '1e308', '-1e308', '1e-320', '0', '-0', '-1', '1 GHz', '2.2k', '100n', '3.3e9',
      '1kk', '1 THz', '1e9999', '-1e9999', 'a' * 5000, '\x00', '‮', 'lowpass', 'bandpass', 'highpass', 'bandstop', 'cauer',
      'chebyshev', 'butterworth', 'bessel', 'elliptic', 'microstrip', 'coplanar', 'coax', 'stripline', 'rectwaveguide', 'twistedpair',
      'cpw', 'pi', 'tee', 'bridged', 'wilkinson', 'analyze', 'synthesize', 'qucsator', 'xyce']
WN = [0, -0.0, 1, -1, 2, 3, 19, 20, 21, 100, 1000, 1e9, 2.4e9, 1e15, 1e300, -1e300, 5e-324, 2**31, 2**63, -2**31, 1e-30, 0.5, 2**53 + 1]


def val(r, schema, key, depth=0):
    t = schema.get('type') if isinstance(schema, dict) else None
    if isinstance(schema, dict) and 'enum' in schema and r.random() < 0.7: return r.choice(schema['enum'])
    if isinstance(t, list): t = r.choice(t)
    if t is None or r.random() < 0.15: t = r.choice(['string', 'number', 'integer', 'boolean', 'array', 'object', 'null'])
    if t == 'string': return r.choice(WS)
    if t in ('number', 'integer'): return r.choice(WN) if r.random() < 0.7 else r.uniform(-10, 10) * 10 ** r.randint(-12, 12)
    if t == 'boolean': return r.random() < 0.5
    if t == 'null': return None
    if t == 'array':
        if depth > 3: return []
        items = schema.get('items', {}) if isinstance(schema, dict) else {}
        n = r.choice([0, 1, 2, 3, 5, 40]) if r.random() < 0.97 else 5000
        return [val(r, items if isinstance(items, dict) else {}, key, depth + 1) for _ in range(n)]
    if t == 'object':
        if depth > 3: return {}
        props = schema.get('properties', {}) if isinstance(schema, dict) else {}
        if props: return {k: val(r, v, k, depth + 1) for k, v in props.items() if r.random() < 0.7}
        keys = ['er', 'h', 't', 'tand', 'rho', 'd', 'name', 'gain', 'nf', 'oip3', 'w', 'l', 'f', 'z0', 'D', 'a', 'b']
        return {r.choice(keys): val(r, {}, 'v', depth + 1) for _ in range(r.randint(0, 6))}
    return None


def spec(r, prog):
    tool, keys = PROGS[prog]
    sch = schemas.get(tool, {})
    props = dict(sch.get('properties', {}))
    for k in keys: props.setdefault(k, {})
    s = {k: val(r, v, k) for k, v in props.items() if r.random() < 0.6}
    if r.random() < 0.05: s[r.choice(list(props))] = json.loads('[' * 3000 + ']' * 3000) if r.random() < 0.5 else 'x'
    return s


def one(k):
    r = random.Random(seed * 7919 + k)
    prog = r.choice(sorted(paths))
    c = r.random()
    if c < 0.04: data = r.choice([b'', b'{', b'[]', b'null', b'"x"', b'{"a":', b'\xff\xfe', b'{}' * 1000, b'[' * 100000])
    else:
        # (at most 4 MB: arrays of thousands inside arrays of thousands made a 1 GB spec, which is the parser's time, not a hang)
        while True:
            data = json.dumps(spec(r, prog), default=str).encode()
            if len(data) < 4 << 20: break
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', HOME=os.path.join(out, 'home'), ASAN_OPTIONS='detect_leaks=0',
               UBSAN_OPTIONS='print_stacktrace=1')
    t0 = time.time()
    try:
        p = subprocess.run([paths[prog], '--json'], input=data, capture_output=True, timeout=30, env=env, cwd=out)
        rc, err, so = p.returncode, p.stderr.decode('utf-8', 'replace'), p.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        rc, err, so = 'HANG', (e.stderr or b'').decode('utf-8', 'replace'), ''
        # (one more time, alone and longer: on a machine this busy a slow start is no hang)
        try:
            p = subprocess.run([paths[prog], '--json'], input=data, capture_output=True, timeout=120, env=env, cwd=out)
            rc, err, so = p.returncode, p.stderr.decode('utf-8', 'replace'), p.stdout.decode('utf-8', 'replace')
        except subprocess.TimeoutExpired:
            pass
    kind = None
    if rc == 'HANG': kind = 'hang'
    elif 'ERROR: AddressSanitizer' in err or 'runtime error:' in err: kind = 'san'
    elif isinstance(rc, int) and rc < 0: kind = 'crash'
    elif rc == 0:
        try: json.loads(so.strip().splitlines()[-1])
        except Exception: kind = 'noanswer'
    return k, prog, data, rc, err, so, kind, time.time() - t0


seen = {}; stats = {}
end = time.time() + seconds; k = 0
with ThreadPoolExecutor(jobs) as ex:
    futs = set()
    while time.time() < end or futs:
        while time.time() < end and len(futs) < jobs * 2:
            futs.add(ex.submit(one, k)); k += 1
        done = [f for f in futs if f.done()]
        for f in done:
            futs.discard(f)
            kk, prog, data, rc, err, so, kind, dt = f.result()
            st = stats.setdefault(prog, {}); st[kind or 'ok'] = st.get(kind or 'ok', 0) + 1
            if not kind: continue
            lines = [l for l in err.splitlines() if ('#' in l and ' in ' in l) or 'runtime error' in l]
            sig = hashlib.sha1((prog + kind + '\n'.join(l.split(' (')[0][-100:] for l in lines[:4])).encode()).hexdigest()[:8]
            if sig in seen: seen[sig] += 1; continue
            seen[sig] = 1
            base = os.path.join(out, f'{kind}-{prog}-{sig}')
            open(base + '.json', 'wb').write(data)
            open(base + '.txt', 'w').write(f'case {kk} rc={rc} {dt:.1f}s\n{err[-20000:]}\n---stdout\n{so[-2000:]}')
            print(f'NEW {kind} {prog} rc={rc} case {kk} -> {base}', flush=True)
        if not done: time.sleep(0.02)
print(json.dumps(stats), flush=True)
print(f'{k} specs, {len(seen)} distinct events', flush=True)

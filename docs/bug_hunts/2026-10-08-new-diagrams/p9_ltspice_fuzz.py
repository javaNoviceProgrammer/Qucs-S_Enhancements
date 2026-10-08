"""Damaged LTspice schematics (.asc) through import_netlist and get_netlist on the ASan build: mutants of the test's sample, seeded."""
import os, random, shutil, sys, time, mcp
seed, count = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
here = os.path.dirname(os.path.abspath(__file__))
base = open(here + '/asc_seeds/rc.asc', encoding='utf-8').read().splitlines()
ODD = ['2147483647', '-2147483648', '99999999999999999999', 'nan', 'inf', '1e308', '-0', '', 'x', '0x10', '3.5', '-7']
EXTRA = ['SYMBOL res 0 0 R0', 'SYMATTR InstName', 'SYMATTR Value', 'FLAG 0 0', 'WIRE 1 2', 'WINDOW', 'TEXT 0 0 Left 2 !', 'TEXT 0 0',
         'SYMBOL voltage 48 64 R0\nSYMATTR InstName V1', 'SYMATTR InstName R1', 'SYMBOL', 'SYMBOL ../../../etc/passwd 0 0 R0', 'SYMBOL res 1e9 -1e9 M270',
         'SYMATTR Value ' + 'k' * 100000, 'SYMATTR Value {a b} "q" \x00 ;', 'SYMATTR InstName R 1', 'FLAG 48 80 out\nFLAG 48 80 in', 'WIRE 5 5 5 5',
         'SYMATTR SpiceLine L=1u W=1u', 'SYMATTR Prefix X', 'IOPIN 240 80 Out', 'BUSTAP 0 0 16 0', 'LINE Normal 0 0 10 10', 'DATAFLAG 0 0 ""',
         'TEXT 0 0 Left 2 !.include ../../x.lib', 'TEXT 0 0 Left 2 !.lib "C:\\\\Program Files\\\\LTC\\\\lib\\\\cmp\\\\standard.dio"', 'TEXT 0 0 Left 2 !.step param R 1 10 1']
def mutate():
    lines = list(base)
    for _ in range(rnd.randint(1, 5)):
        k = rnd.randrange(8)
        i = rnd.randrange(len(lines)) if lines else 0
        if not lines: lines.append(rnd.choice(EXTRA)); continue
        if k == 0:
            t = lines[i].split(' ')
            j = rnd.randrange(len(t)); t[j] = rnd.choice(ODD); lines[i] = ' '.join(t)
        elif k == 1: del lines[i]
        elif k == 2: lines[i:i] = [lines[i]] * rnd.choice([2, 3, 50])
        elif k == 3: j = rnd.randrange(len(lines)); lines[i], lines[j] = lines[j], lines[i]
        elif k == 4: lines.insert(i, rnd.choice(EXTRA))
        elif k == 5: lines[i] = lines[i].replace('R0', rnd.choice(['R45', 'M999', 'R', 'M', 'R-90', 'X90']))
        elif k == 6: lines = lines[:i]
        else: lines[i] = lines[i][:rnd.randrange(len(lines[i]) + 1)]
    text = '\n'.join(lines) + rnd.choice(['\n', '', '\r\n'])
    return text.encode('utf-16') if rnd.random() < 0.15 else text.encode('utf-8', 'surrogatepass')
out = os.environ['HUNT_RUNS'] + f'/p9-{seed}'
os.makedirs(out + '/keep', exist_ok=True)
s = mcp.Server(f'p9s{seed}')
shutil.copy(here + '/asc_seeds/mysub.asy', s.ws + '/mysub.asy')
crashes, slow, errs, ok = 0, 0, 0, 0
for n in range(count):
    data = mutate()
    f = f'{s.ws}/m{n}.asc'; open(f, 'wb').write(data)
    t0 = time.time()
    try:
        r = s.call('import_netlist', {'file': f, 'save_as': f'm{n}.sch', 'replace': True, 'symbols': s.ws})
        if s.last_error: errs += 1
        else:
            ok += 1
            s.call('get_netlist', {})
            s.call('close_document', {})
        if time.time() - t0 > 10: slow += 1; shutil.copy(f, out + '/keep/'); print(f'slow {n}: {time.time() - t0:.1f}s', flush=True)
    except Exception as e:
        crashes += 1
        shutil.copy(f, out + '/keep/')
        err = open(s.root + '/server.err', errors='replace').read()
        print(f'CRASH at mutant {n}:', [l for l in err.splitlines() if 'Sanitizer' in l or 'runtime error' in l or 'SUMMARY' in l][:4] or err[-600:], flush=True)
        s = mcp.Server(f'p9s{seed}-{n}')
        shutil.copy(here + '/asc_seeds/mysub.asy', s.ws + '/mysub.asy')
err = open(s.root + '/server.err', errors='replace').read()
san = [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l]
print(f'seed {seed}: {count} mutants, imported {ok}, refused {errs}, crashes {crashes}, slow {slow}; sanitizer lines in the last server: {san[:5]}')
s.close()

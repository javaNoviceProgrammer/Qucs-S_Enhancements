"""p13: damaged library files through import_library, list_libraries, describe_part, add_component, get_netlist and check_schematic,
on the ASan/UBSan app (QUCS=.../build-asan/...). Seeds: installed libraries and the hunt's generated ones (seeds/). N mutants, seed SEED."""
import os, re, sys, json, random, shutil, time, mcp

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALLED = mcp.REPO + '/qucs-s-26.1.1/library'
SEEDS = [INSTALLED + f'/{n}.lib' for n in ('Neon', 'Crystal', 'BF998', 'BJT_Darlington', 'DualGateMOSFET', 'RC', 'Thermistor', 'OpAmp_544')]
SEEDS += [HERE + f'/seeds/{n}.lib' for n in ('Hier', 'Sp', 'Models')]
N = int(os.environ.get('N', '150'))
rng = random.Random(int(os.environ.get('SEED', '7')))
INSERTS = ['<Component X>', '</Component>', '<Spice>', '</Spice>', '<Model>', '</Model>', '<Symbol>', '</Symbol>',
           '<ModelIncludes "', '<SpiceAttach "', '<SpiceAttach "a.lib" "b.osdi" "c.va">', '<DefaultSymbol>', '</DefaultSymbol>',
           '<Description>', '</Description>', '<AlwaysLoadOSDI>', '<.PortSym 0 0 1 0>', '<.PortSym 0 0 99999999 0>',
           '<.ID -20 -30 >', '<Line', '.SUBCKT', '.ENDS', '.SUBCKT x gnd', '<Qucs Library 999999999.0.0 "x">', '"' * 3, '<' * 5, '>' * 5]

def mutate(text):
    lines = text.split('\n')
    for _ in range(rng.randint(1, 4)):
        k = rng.randrange(10)
        i = rng.randrange(len(lines)) if lines else 0
        if k == 0 and text: return text[:rng.randrange(len(text))]                    # cut
        if k == 1 and lines: del lines[i]
        elif k == 2 and lines: lines.insert(i, lines[i])
        elif k == 3 and lines: lines[i] = lines[i].replace('>', '', 1)
        elif k == 4 and lines: lines[i] = lines[i].replace('"', '', 1)
        elif k == 5: lines.insert(i, rng.choice(INSERTS))
        elif k == 6 and lines and lines[i]:
            j = rng.randrange(len(lines[i])); lines[i] = lines[i][:j] + chr(rng.randrange(32, 127)) + lines[i][j + 1:]
        elif k == 7 and len(lines) > 1:
            j = rng.randrange(len(lines)); lines[i], lines[j] = lines[j], lines[i]
        elif k == 8 and lines: lines[i] = lines[i] + 'x' * rng.choice([0, 1000, 100000])
        elif k == 9 and lines: lines[i] = re.sub(r'\d+', lambda m: rng.choice(['-1', '0', '99999999999', '2147483648', 'nan']), lines[i], count=1)
    return '\n'.join(lines)

# (The command check on: a library's SPICE text and files are read for commands.)
s = mcp.Server('p13', settings_ini=f'[General]\nCheckCommands=true\nQucsator={mcp.REPO}/build/qucsator_rf/src/qucsator_rf\n')
d = s.ws + '/fz_prj'
s.call('new_project', {'name': 'fz'})
s.call('open_project', {'name': 'fz'})
src = s.root + '/src'
os.makedirs(src, exist_ok=True)
t0 = time.time()
slow, refused = [], 0
for n in range(N):
    seed = rng.choice(SEEDS)
    base = os.path.basename(seed)[:-4]
    text = mutate(open(seed, encoding='utf-8', errors='replace').read())
    name = f'F{n}'
    open(f'{src}/{name}.lib', 'w').write(text.replace(f'"{base}"', f'"{name}"', 1))
    if os.path.isdir(seed[:-4]): shutil.copytree(seed[:-4], f'{src}/{name}', dirs_exist_ok=True)
    try:
        t = time.time()
        r = s.call('import_library', {'path': f'{src}/{name}.lib', 'replace': True})
        if s.last_error: refused += 1; continue
        parts = [p.get('part') for p in r.get('parts', [])][:2] if isinstance(r, dict) else []
        s.call('list_libraries', {'library': name})
        s.call('new_document', {})
        for i, part in enumerate(parts):
            s.call('describe_part', {'library': name, 'part': part})
            s.call('add_component', {'type': 'Lib', 'name': f'X{i}', 'x': 200 + 200 * i, 'y': 200, 'properties': {'Lib': name, 'Comp': part}})
        s.call('get_netlist', {})
        s.call('check_schematic', {})
        s.call('close_document', {'discard': True})
        if time.time() - t > 10: slow.append((n, round(time.time() - t, 1)))
    except RuntimeError as e:
        print('server died at', n, seed, str(e)[-1500:])
        open(f'{s.root}/crash_{n}.lib', 'w').write(text)
        sys.exit(1)
s.close()
err = open(s.root + '/server.err').read()
reports = re.findall(r'(ERROR: AddressSanitizer[^\n]*|runtime error[^\n]*|LeakSanitizer[^\n]*)', err)
print(f'{N} mutants in {time.time() - t0:.0f} s; {refused} refused by import; slow (>10 s): {slow}')
print('sanitizer reports:', len(reports), sorted(set(reports))[:10])

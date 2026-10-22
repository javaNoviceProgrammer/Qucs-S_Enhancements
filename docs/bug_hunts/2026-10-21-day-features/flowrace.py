# Process flows built in the background, interrupted: closed, edited, stopped, built again while
# building; the server's health checked after each (a crash: EOF; a sanitizer report in its log).
import json, os, sys, time
HERE = '/Users/meisam/git/Qucs-S_Enhancements/docs/bug_hunts/2026-10-21-fuzz'
seed = int(sys.argv[1]); count = int(sys.argv[2])
sys.argv = ['fuzz_kinds.py', 'process', str(seed), '0']
src = open(os.path.join(HERE, 'fuzz_kinds.py')).read().split('s = Server(); pos =')[0]
__file__ = os.path.join(HERE, 'fuzz_kinds.py')
exec(compile(src, 'fuzz_kinds_head', 'exec'))
s = Server(); pos = os.path.getsize(errpath)
def call(name, args, limit=120):
    t = time.time(); r, took = s.call(name, args, limit)
    txt = r if isinstance(r, str) else json.dumps(r.get('result', r))[:160]
    return txt, time.time() - t
def report():
    global pos
    with open(errpath, errors='replace') as f:
        f.seek(pos); new = f.read(); pos = f.tell()
    return [l for l in new.split('\n') if 'ERROR: AddressSanitizer' in l or 'runtime error' in l or 'ASSERT' in l][:3]
for i in range(count):
    PLAUSIBLE = True
    model = process_model()
    # A long anneal so that a build takes a while.
    model['components'][0]['geometry'][0]['features'] = [{'type': 'process/substrate', 'material': 'Silicon', 'thickness': '2[um]', 'orientation': '100'},
        {'type': 'process/implant'}, {'type': 'process/diffuse', 'temperature': '1100[degC]', 'time': '120[min]'}]
    model['components'][0]['geometry'][0].update({'width': '4', 'dx': '10[nm]', 'dy': '2[nm]', 'depth': '2'})
    path = os.path.join(WS, f'race{i % 5}.qfem'); open(path, 'w').write(json.dumps(model))
    script = [('open_document', {'path': path}), ('fem_build', {'path': path, 'timeout': 1}),
              ('fem_edit', {'path': path, 'tag': 'pf1', 'props': {'width': '8'}}), ('fem_edit', {'path': path, 'tag': 'pf1', 'props': {'depth': '1.5'}}),
              ('fem_build', {'path': path, 'timeout': 1}), ('fem_stop', {'path': path}),
              ('fem_build', {'path': path, 'timeout': 1}), ('close_document', {'path': path, 'unsaved': 'discard'}),
              ('open_document', {'path': path}), ('fem_build', {'path': path, 'timeout': 1}),
              ('close_document', {'path': path, 'unsaved': 'discard'}), ('list_documents', {})]
    out = []
    for name, args in script:
        txt, took = call(name, args)
        out.append(f'{name} {took:.1f}s {"EOF" if txt == "EOF" else ""}' + (' ERR ' + txt[:90] if 'isError": true' in txt else ''))
        if txt in ('EOF', 'HANG') or (isinstance(txt, str) and txt.startswith('HANG')):
            print(i, 'SERVER', txt, out, flush=True); s = Server(); pos = os.path.getsize(errpath); break
    bad = report()
    if i == 0: print(out)
    print(i, "ok" if not bad else 'SANITIZER ' + str(bad), '; '.join(o for o in out if float(o.split()[1][:-1]) > 5 or 'EOF' in o), flush=True)

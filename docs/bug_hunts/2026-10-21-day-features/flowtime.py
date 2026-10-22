import json, os, sys, time
HERE = '/Users/meisam/git/Qucs-S_Enhancements/docs/bug_hunts/2026-10-21-fuzz'
sys.argv = ['fuzz_kinds.py', 'process', '1', '0']
src = open(os.path.join(HERE, 'fuzz_kinds.py')).read().split('s = Server(); pos =')[0]
__file__ = os.path.join(HERE, 'fuzz_kinds.py')
exec(compile(src, 'fuzz_kinds_head', 'exec'))
s = Server()
model = {'format': 'qucs-s multiphysics 1', 'components': [{'unit': 'um', 'geometry': [{'type': 'process', 'tag': 'pf1', 'label': 'flow', 'width': '4', 'dx': '10[nm]', 'dy': '2[nm]', 'depth': '2',
    'features': [{'type': 'process/substrate', 'material': 'Silicon', 'thickness': '2[um]', 'orientation': '100'}, {'type': 'process/implant'},
                 {'type': 'process/diffuse', 'temperature': '1100[degC]', 'time': '120[min]'}]}], 'materials': [{'material': 'Silicon', 'selection': {'all': True}}], 'mesh': {'size': 'normal'}}], 'studies': []}
path = os.path.join(WS, 'flowtime.qfem'); open(path, 'w').write(json.dumps(model))
for name, args in [('open_document', {'path': path}), ('fem_build', {'path': path, 'timeout': 300}), ('fem_edit', {'path': path, 'node': 'flow', 'properties': {'width': '8'}}),
                   ('fem_build', {'path': path, 'timeout': 0.1}), ('fem_stop', {'path': path}), ('fem_build', {'path': path, 'timeout': 300})]:
    t = time.time(); r, took = s.call(name, args, 600)
    print(name, f'{time.time()-t:.2f}s', (r if isinstance(r, str) else json.dumps(r.get('result', r)))[:230], flush=True)

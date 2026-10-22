import sys, json; sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
exec(open('/tmp/qucs-hunt/hunt2/f6.py').read().split('layers = ')[0])
for at in ("5", "-1", "1", "0.999", "1/0", "nan"):
    r = run(junction("1d", [], "0.1", {"h1d": "5[nm]", "at": at}), [('fem_solve', {'study': 's'})], tag='f51', width=0)
    e, t = r[0]
    print('at', at, '->', t[:220] if e else ('ok', json.loads(t)['globals'].get('semi.I_a'), [l[:120] for l in json.loads(t)['log'] if 'solved in 1D along' in l]))

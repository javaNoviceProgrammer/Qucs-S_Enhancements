# a flag as an expression that is not finite, has a unit, or is a denormal: true, false or refused?
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
for v in ("1", "0", "0/0", "1/0", "-1/0", "1[V]", "0[V]", "1e-320", "ex", "ex*0/0"):
    m = well([{"name": "ex", "expression": "0/0"}], [dict(spec, energies="linspace(1.40[eV], 1.50[eV], 3)", excitons=v)])
    with contextlib.redirect_stdout(io.StringIO()):
        r = run(m, [('fem_solve', {'study': 's'})], tag='f76', width=0)
    e, t = r[0]
    if e: print('%-8s refused: %s' % (v, t[:220])); continue
    eb = re.search(r'"qw.Eb":"([^"]*)"', t)
    print('%-8s solved, qw.Eb %s' % (v, eb.group(1) if eb else '-'))

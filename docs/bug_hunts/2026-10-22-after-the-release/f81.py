# fem_evaluate a spectrum at a photon energy between its grid's, beyond it, and by wavelength
import sys, json, re, contextlib, io
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run
from f2 import well, spec
m = well([], [dict(spec, energies="linspace(1.40[eV], 1.60[eV], 5)")])
fs = ["1.45[eV]", "1.5[eV]", "1.4[eV]", "826.6[nm]", "3[eV]", "1.5 eV", "1.5eV", "x", "1.5e-3[V]"]
calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': 'qw.alphaTE', 'frequency': f}) for f in fs]
with contextlib.redirect_stdout(io.StringIO()):
    r = run(m, calls, tag='f81', width=0)
for f, (e, t) in zip(fs, r[1:]):
    try:
        j = json.loads(t); print('%-9s' % f, [(x.get('name', '')[:30], x['value']) for x in j['values']], j.get('note', '')[:160])
    except Exception: print('%-9s' % f, 'ERR' if e else 'TEXT', t[:220])

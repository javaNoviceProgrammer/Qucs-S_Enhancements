import sys, time; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
import json
p = Probe(sys.argv[1] + '/p2')
base = '  <R R1 1 200 270 15 -26 0 1 "50" 1>\n  <GND * 1 200 300 0 0 0 0>\n  <GND * 1 120 330 0 0 0 0>\n  <.TR TR1 1 300 100 0 64 0 0 "lin" 1 "0" 1 "50 ns" 1 "5001" 0>\n'
wires = '  <120 270 200 240 "out" 160 220 0 "">\n'
cases = {
 'ok':      '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0',
 'order1':  '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "1" 1 "" 0 "NRZ" 0',
 'order40': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "40" 1 "" 0 "NRZ" 0',
 'tbit0':   '"0 V" 1 "1 V" 1 "0" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0',
 'tbitneg': '"0 V" 1 "1 V" 1 "-1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0',
 'trbig':   '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "5 ns" 0 "5 ns" 0 "7" 1 "" 0 "NRZ" 0',
 'seed0':   '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "0" 0 "NRZ" 0',
 'seedbig': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "99999" 0 "NRZ" 0',
 'pam4odd': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "PAM4" 0',
}
for name, props in cases.items():
    comp = '  <vPRBS V1 1 120 300 18 -26 0 1 %s>\n' % props
    p.open(name + '.sch', sch(comp + base, wires))
    t0 = time.time()
    e, out = p.call('simulate', {'timeout': 30})
    try:
        j = json.loads(out)
        summary = {k: j.get(k) for k in ('succeeded', 'variable count', 'errors')}
        if not j.get('errors'): summary['last'] = (j.get('last lines') or '')[-160:].replace('\n', ' / ')
    except Exception:
        summary = out[:200]
    vmax = ''
    if isinstance(summary, dict) and summary.get('succeeded'):
        e, d = p.call('get_dataset', {'variables': ['tran.v(out)'], 'measure': []})
        try:
            v = json.loads(d)['variables'][0]; vmax = 'min %s max %s' % (v.get('min'), v.get('max'))
        except Exception: vmax = d[:100]
    print('%-8s %.1fs %s %s' % (name, time.time() - t0, json.dumps(summary)[:300], vmax))
    p.call('close_document', {'unsaved': 'discard'})
p.close()

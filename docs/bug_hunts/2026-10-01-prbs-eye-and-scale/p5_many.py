import sys, time, json; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
from chain import chain
p = Probe(sys.argv[1] + '/p5')
param = '  <SpicePar SpicePar1 1 600 -400 -29 19 0 0 "RV=1k" 1>\n'
tr = '  <.TR TR1 1 0 -200 0 64 0 0 "lin" 1 "0" 1 "1 us" 1 "11" 0>\n'
cases = {
 'eqn':   param + tr + '  <Eqn Eqn1 1 300 -300 -30 15 0 0 "twice=v(n5)*2" 1 "yes" 0>\n',
 'sweep': param + tr + '  <.SW SW1 1 200 -300 0 77 0 0 "TR1" 1 "lin" 1 "RV" 1 "1k" 1 "3k" 1 "3" 1>\n',
 'ac':    param + '  <.AC AC1 1 0 -200 0 33 0 0 "lin" 1 "1 kHz" 1 "10 kHz" 1 "3" 1 "no" 0>\n  <Vac V2 1 -100 60 18 -26 0 1 "1 V" 1 "1 kHz" 0 "0" 0 "0" 0>\n',
 'dcsw':  param + '  <.DC DC1 1 0 -200 0 33 0 0>\n  <.SW SW1 1 200 -300 0 77 0 0 "DC1" 1 "lin" 1 "RV" 1 "1k" 1 "3k" 1 "3" 1>\n',
}
for name, extra in cases.items():
    comps, wires = chain(1000, extra)
    print(name, 'open:', p.open(name + '.sch', sch(comps, wires))[1][-40:])
    e, net = p.call('get_netlist')
    longest = max(len(l.split()) for l in net.splitlines())
    saves = sum(1 for l in net.splitlines() if l.startswith('save '))
    writes = [l[:60] for l in net.splitlines() if l.startswith(('write', 'print'))]
    t0 = time.time()
    e, out = p.call('simulate', {'timeout': 120}) 
    try:
        j = json.loads(out); s = {k: j.get(k) for k in ('succeeded', 'variable count', 'errors')}
        if j.get('variables'): s['first'] = j['variables'][:3]
    except Exception: s = out[:300]
    print('%-6s longest %d words, %d saves, %s | %.1fs %s' % (name, longest, saves, writes[:3], time.time() - t0, json.dumps(s)[:400]))
    p.call('close_document', {'unsaved': 'discard'})
p.close()

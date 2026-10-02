import sys, json; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
p = Probe(sys.argv[1] + '/p11')
def tr(start, stop, points, maxstep='0', typ='lin', active='1'):
    return '  <.TR TR1 %s 300 0 0 64 0 0 "%s" 1 "%s" 1 "%s" 1 "%s" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "%s" 0>\n' % (active, typ, start, stop, points, maxstep)
def vp(rise, fall=None):
    fall = rise if fall is None else fall
    return '  <Vpulse V1 1 100 60 18 -26 0 1 "0 V" 1 "1 V" 1 "1 us" 1 "6 us" 1 "%s" 0 "%s" 0>\n  <GND * 1 100 90 0 0 0 0>\n' % (rise, fall)
wires = '  <100 30 100 30 "out" 110 10 0 "">\n'
cases = [
 ('start after 0', vp('1 ns'), tr('5 us', '6 us', '11')),
 ('points expr', vp('1 ns'), tr('0', '20 us', '{N}')),
 ('stop expr', vp('1 ns'), tr('0', '{T}', '2001')),
 ('points 1e3', vp('1 ns'), tr('0', '20 us', '1e3')),
 ('points 2001.5', vp('1 ns'), tr('0', '20 us', '2001.5')),
 ('maxstep neg', vp('1 ns'), tr('0', '20 us', '2001', '-1 ns')),
 ('rise 0 fall 1n', vp('0', '1 ns'), tr('0', '20 us', '2001')),
 ('log type', vp('1 ns'), tr('1 ns', '20 us', '2001', '0', 'log')),
 ('two TR', vp('1 ns'), tr('0', '20 us', '2001') + tr('0', '20 us', '200001').replace('TR1', 'TR2')),
 ('negative rise', vp('-1 ns'), tr('0', '20 us', '2001')),
]
for name, comps, t in cases:
    p.open('s.sch', sch(comps + t, wires))
    e, out = p.call('check_schematic')
    j = json.loads(out)
    notes = [n['message'][:150] for n in j.get('notes', []) if 'time step' in n['message']]
    other = [x['message'][:100] for x in j.get('errors', []) + j.get('warnings', [])]
    print('%-15s %s %s' % (name, notes, other[:2]))
    p.call('close_document', {'unsaved': 'discard'})
p.close()

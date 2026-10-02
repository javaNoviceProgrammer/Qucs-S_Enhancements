import sys; sys.path.insert(0, sys.argv[1])
from probe import Probe, sch
import json
p = Probe(sys.argv[1] + '/p1')
base = '  <GND * 1 120 330 0 0 0 0>\n  <R R1 1 200 270 15 -26 0 1 "50" 1>\n  <GND * 1 200 300 0 0 0 0>\n  <.TR TR1 1 300 100 0 64 0 0 "lin" 1 "0" 1 "50 ns" 1 "5001" 0>\n'
wires = '  <120 270 200 240 "out" 160 220 0 "">\n'
cases = {
 'order1':  '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "1" 1 "" 0 "NRZ" 0',
 'order40': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "40" 1 "" 0 "NRZ" 0',
 'orderX':  '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "seven" 1 "" 0 "NRZ" 0',
 'tbit0':   '"0 V" 1 "1 V" 1 "0" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0',
 'tbitneg': '"0 V" 1 "1 V" 1 "-1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "NRZ" 0',
 'trbig':   '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "5 ns" 0 "5 ns" 0 "7" 1 "" 0 "NRZ" 0',
 'seed0':   '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "0" 0 "NRZ" 0',
 'seedneg': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "-5" 0 "NRZ" 0',
 'seedbig': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "99999" 0 "NRZ" 0',
 'pam4odd': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "PAM4" 0',
 'codingX': '"0 V" 1 "1 V" 1 "1 ns" 1 "0" 0 "" 0 "" 0 "7" 1 "" 0 "pam8" 0',
}
for name, props in cases.items():
    comp = '  <vPRBS V1 1 120 300 18 -26 0 1 %s>\n' % props
    e, t = p.open(name + '.sch', sch(comp + base, wires))
    e, net = p.call('get_netlist')
    line = [l for l in net.splitlines() if l.lower().startswith('v1 ')]
    e, chk = p.call('check_schematic')
    j = json.loads(chk)
    found = [x['message'][:110] for x in j.get('errors', []) + j.get('warnings', []) if 'V1' in x['message'] or 'PRBS' in x['message'] or 'Tbit' in x['message'] or 'Order' in x['message']]
    print('%-8s %s | %s' % (name, line[0] if line else '(no line) ' + net[:100].replace('\n', ' '), found))
    p.call('close_document', {'unsaved': 'discard'})
p.close()

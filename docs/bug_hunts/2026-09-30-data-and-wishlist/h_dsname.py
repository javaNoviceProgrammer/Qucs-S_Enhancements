# A Data Set that is not name.dat (Document Settings takes any text): what a simulation writes, and the traces.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
TR = '  <.TR TR1 1 200 40 0 71 0 0 "lin" 1 "0" 1 "1 ms" 1 "11" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "0" 0>\n'
for ds in ('run.csv', 'run', 'sub/run.dat', '../up.dat', 'run.dat.ngspice'):
    ws = HERE + '/ws_dsn'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws + '/sub')
    s = Server(ws)
    txt = ('<Qucs Schematic 26.1.4>\n<Properties>\n  <DataSet=%s>\n  <DataDisplay=amp.dpl>\n</Properties>\n<Components>\n' % ds +
           '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <R R1 1 100 60 15 -26 0 1 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n'
           '  <GND * 1 0 90 0 0 0 0>\n' + TR + '</Components>\n<Wires>\n  <0 30 100 30 "out" 50 10 0 "">\n  <0 90 100 90 "" 0 0 0 "">\n</Wires>\n'
           '<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
    open(ws + '/amp.sch', 'w').write(txt)
    s.call('open_document', {'path': ws + '/amp.sch'})
    r = s.call('simulate', {'path': 'amp.sch'}, ok=False)
    d = s.call('add_diagram', {'path': 'amp.sch', 'traces': ['out']}, ok=False)
    files = sorted(os.path.relpath(os.path.join(dp, f), ws) for dp, dn, fn in os.walk(HERE + '/ws_dsn/..') for f in fn if 'up.dat' in f or os.path.join(dp, f).startswith(ws)) 
    print(repr(ds), '| sim:', r.get('succeeded') if isinstance(r, dict) else r[:120], r.get('dataset', '') if isinstance(r, dict) else '', '| trace:',
          [(t['variable'], t.get('points'), (t.get('no data') or '')[:60]) for t in d['traces']] if isinstance(d, dict) else d[:120])
    print('    files:', [f for f in files if 'spice4qucs' not in f and not f.endswith('.sch')])
    s.close()

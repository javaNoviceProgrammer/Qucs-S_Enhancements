"""An AC analysis added to the RC example: do its own diagrams (tran.v(in), ac.s of the FFT) keep their data? And the probe/marker/tools' names."""
import json, sys, mcp
from common import rc, show
s = mcp.Server('p14')
p, sim = rc(s)
g = s.call('get_schematic', {})
show('before: the example diagrams', [[(t['variable'], t.get('points'), t.get('no data', '')[:60]) for t in d['traces']] for d in g['diagrams']], 800)
s.call('add_analysis', {'kind': 'ac', 'properties': {'Start': '10 Hz', 'Stop': '1 MHz', 'Points': '101'}})
s.call('save_document', {})
sim = s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_schematic', {})
show('after an AC analysis is added: the same diagrams', [[(t['variable'], t.get('points'), t.get('no data', '')[:90]) for t in d['traces']] for d in g['diagrams']], 1200)
show('get_dataset tran.v(out)', s.call('get_dataset', {'variables': ['tran.v(out)'], 'points': 0}), 500)
show('get_dataset v(out)', s.call('get_dataset', {'variables': ['v(out)'], 'points': 0}), 500)
s.close()

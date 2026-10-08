"""Types made for a transient given an AC sweep (and a Bode/Nichols given a transient): refused, warned, or drawn as if right?"""
import mcp
from common import rc, show
s = mcp.Server('p26')
p, sim = rc(s)
s.call('add_analysis', {'kind': 'ac', 'from': '10 Hz', 'to': '1 MHz', 'points': 101}); s.call('delete', {'components': ['FFT1']})
s.call('save_document', {}); sim = s.call('simulate', {'simulator': 'ngspice'})
print('vars', [v['name'] for v in s.call('get_dataset', {'points': 0}).get('variables', [])])
for t, tr in (('spectrum', ['ac1.ac.v(out)']), ('spectrogram', ['ac1.ac.v(out)']), ('bathtub', ['ac1.ac.v(out)']), ('constellation', ['ac1.ac.v(in)', 'ac1.ac.v(out)']),
              ('bode', ['tr1.tran.v(out)']), ('nichols', ['tr1.tran.v(out)']), ('pole_zero', ['tr1.tran.v(out)']), ('box_plot', ['ac1.ac.v(out)'])):
    r = s.call('add_diagram', {'type': t, 'traces': tr})
    keep = {k: r.get(k) for k in ('analyses', 'margins', 'roots', 'constellation', 'box_plot', 'note') if isinstance(r, dict) and r.get(k) is not None}
    show(f'{t} of {tr} err={int(s.last_error)}', keep or r, 500)
r = s.call('add_diagram', {'type': 'box_plot', 'traces': ['tr1.tran.v(out)'], 'box_plot': {'at': 0.003}})
show('box_plot at 3 ms on a trace of one curve', r.get('box_plot'), 400)
s.close()

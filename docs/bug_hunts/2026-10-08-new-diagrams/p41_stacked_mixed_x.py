"""A stacked diagram of a transient (pane 1) and a spectrum (pane 2, the FFT's): one x axis for seconds and hertz - what the marker reads."""
import mcp
from common import rc, show
s = mcp.Server('p41')
p, sim = rc(s)
r = s.call('add_diagram', {'type': 'stacked', 'traces': ['tran.v(out)', {'variable': 'ac.v(out)', 'pane': 2}]})
show('added', {k: r.get(k) for k in ('note', 'x_axis')}, 400)
m = s.call('add_marker', {'diagram': r['diagram'], 'at': 0.003, 'trace': 1})
show('marker on the transient at 3 ms', m.get('text') if isinstance(m, dict) else m, 300)
m = s.call('add_marker', {'diagram': r['diagram'], 'at': 1000, 'trace': 2})
show('marker on the spectrum at 1 kHz', m.get('text') if isinstance(m, dict) else m, 300)
s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + '/mixed.png'})
print(s.root)
s.close()

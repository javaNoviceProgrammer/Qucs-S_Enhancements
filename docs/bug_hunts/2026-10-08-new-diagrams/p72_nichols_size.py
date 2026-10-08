"""The Nichols chart's 105 MB SVG: its grid (M and N contours) or its trace? And on the RLC's real loop gain (log AC from 100 Hz). The Bode of the FFT's data, as PNG."""
import os, mcp
from common import rc, show
s = mcp.Server('p72')
p, sim = rc(s)
for label, kw in (('FFT data, grid on', {}), ('FFT data, grid off', {'nichols': {'grid': False}})):
    r = s.call('add_diagram', dict(type='nichols', traces=['ac.v(out)'], **kw))
    s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + '/n.svg'})
    print(f'{label}: svg {os.path.getsize(s.root + "/n.svg")}', flush=True)
r = s.call('add_diagram', {'type': 'nichols', 'traces': []})
s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + '/n0.svg'}); print('no trace, grid on: svg', os.path.getsize(s.root + '/n0.svg'))
b = s.call('add_diagram', {'type': 'bode', 'traces': ['ac.v(out)']})
s.call('export_image', {'diagram': b['diagram'], 'save_as': s.root + '/bode_fft.png'})
print(s.root)
s.close()

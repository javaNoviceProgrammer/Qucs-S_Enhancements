"""The square v(in) drawn at 240 and 2400 wide, x 0 to 1 ms: where does the drawn falling edge start? Release build."""
import mcp
from common import rc, show
s = mcp.Server('p48')
p, sim = rc(s)
for w in (240, 2400):
    d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(in)'], 'width': w, 'height': 200, 'x_axis': {'auto': False, 'from': 0, 'to': 0.001, 'step': 0.0001}})['diagram']
    s.call('export_image', {'diagram': d, 'save_as': s.root + f'/edge_{w}.png', 'scale': 1})
print(s.root)
s.close()

"""The crash of p32, narrowed: a marker on a stacked diagram of some size and pane count (QUCS picks the build)."""
import sys, mcp
from common import rc, show
w, h, panes = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = mcp.Server('p33')
p, sim = rc(s)
r = s.call('add_diagram', {'type': 'stacked', 'traces': ['tran.v(out)', {'variable': 'tran.v(in)', 'pane': panes}], 'panes': panes, 'width': w, 'height': h})
try:
    m = s.call('add_marker', {'diagram': r['diagram'], 'at': 0.003, 'trace': 1})
    print(f'{w}x{h}, {panes} panes: marker placed:', str(m)[:160])
except Exception as e:
    print(f'{w}x{h}, {panes} panes: SERVER DIED -', [l for l in open(s.root + '/server.err', errors='replace').read().splitlines() if 'SEGV' in l or 'runtime error' in l][:2])
s.close()

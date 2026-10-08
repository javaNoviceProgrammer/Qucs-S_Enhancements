"""A marker on a Bode diagram (QUCS picks the build): of a transient, of the FFT's spectrum, of an AC run without a drive; sizes given."""
import sys, mcp
from common import rc, show
trace, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
s = mcp.Server('p34')
p, sim = rc(s)
r = s.call('add_diagram', {'type': 'bode', 'traces': [trace], 'width': w, 'height': h})
try:
    m = s.call('add_marker', {'diagram': r['diagram'], 'at': 0.003 if 'tran' in trace else 1000, 'trace': 1} if len(sys.argv) < 5 else {'diagram': r['diagram'], 'at': 1000})
    print(f'bode of {trace} {w}x{h}: marker:', str(m)[:200])
except Exception as e:
    print(f'bode of {trace} {w}x{h}: SERVER DIED (pid gone) -', [l for l in open(s.root + '/server.err', errors='replace').read().splitlines() if 'SEGV' in l or 'runtime error' in l or 'Segmentation' in l][:2], 'exit', s.p.poll())
s.close()

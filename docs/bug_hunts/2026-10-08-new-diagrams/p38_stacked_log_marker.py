"""A marker on a stacked diagram with a log x axis over data from 0 (the FFT's), and on a rect diagram the same (Release build)."""
import sys, mcp
from common import rc, show
for t, kw in (('stacked', {'x_axis': {'log': True}}), ('rect', {'x_axis': {'log': True}}), ('stacked', {})):
    s = mcp.Server('p38')
    p, sim = rc(s)
    r = s.call('add_diagram', dict(type=t, traces=['ac.v(out)'], **kw))
    try:
        m = s.call('add_marker', {'diagram': r['diagram'], 'at': 1000}); print(f'{t} {kw}: marker placed', str(m)[:100])
    except Exception:
        print(f'{t} {kw}: SERVER DIED, exit', s.p.poll())
    s.close()

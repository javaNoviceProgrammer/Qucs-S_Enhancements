"""F1's reach: a marker placed on a linear x axis over data from 0, then the axis made log (edit_diagram, as the dialog's 'log' box); then moving it; Release build."""
import sys, mcp
from common import rc, show
for t in ('rect', 'bode_like_stacked', 'polar'):
    s = mcp.Server('p39')
    p, sim = rc(s)
    typ = 'stacked' if t == 'bode_like_stacked' else t
    r = s.call('add_diagram', {'type': typ, 'traces': ['ac.v(out)']})
    m = s.call('add_marker', {'diagram': r['diagram'], 'at': 1000})
    try:
        e = s.call('edit_diagram', {'diagram': r['diagram'], 'x_axis': {'log': True}})
        print(f'{t}: x made log with a marker on it:', 'err ' + str(e)[:100] if s.last_error else 'ok')
        e = s.call('edit_marker', {'diagram': r['diagram'], 'marker': 1, 'at': 2000})
        print(f'{t}:   marker moved:', str(e)[:100])
        e = s.call('export_image', {'diagram': r['diagram'], 'save_as': s.root + '/x.png'})
        print(f'{t}:   exported:', 'err ' + str(e)[:80] if s.last_error else 'ok')
    except Exception:
        print(f'{t}: SERVER DIED, exit', s.p.poll())
    s.close()

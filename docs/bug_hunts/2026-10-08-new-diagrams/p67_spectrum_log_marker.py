"""F1 on the spectrum view: its x log (a spectrum's natural axis), a marker placed (Release build)."""
import mcp
from common import rc, show
for kw in ({'x_axis': {'log': True}}, {}):
    s = mcp.Server('p67')
    p, sim = rc(s)
    r = s.call('add_diagram', dict(type='spectrum', traces=['tran.v(out)'], **kw))
    try:
        m = s.call('add_marker', {'diagram': r['diagram'], 'at': 3000})
        print(f'spectrum {kw}: marker', str(m)[:120] if not s.last_error else 'refused: ' + str(m)[:160])
    except Exception:
        print(f'spectrum {kw}: SERVER DIED, exit', s.p.poll())
    s.close()

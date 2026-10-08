"""tune's hold {limits: n} against A2's orphaned limit: R1's values tried with v(in) (1 V) held under 0.9 V on a pane taken away (Release build)."""
import mcp
from common import rc, show
s = mcp.Server('p81')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'stacked', 'traces': ['tran.v(out)', {'variable': 'tran.v(in)', 'pane': 4}], 'panes': 4,
                           'limits': [{'upper': 0.9, 'pane': 4, 'label': 'max'}]})['diagram']
def tune():
    r = s.call('tune', {'component': 'R1', 'values': ['50k', '100k'], 'measure': {'variable': 'tran.v(out)', 'what': 'final'}, 'hold': [{'measure': {'limits': d}, 'max': 0}], 'apply': False})
    return r if s.last_error else str(r)[:700]
show('limit on pane 4 of 4 (FAIL)', tune(), 700)
s.call('edit_diagram', {'diagram': d, 'panes': 2})
show('pane 4 taken away', tune(), 700)
s.close()

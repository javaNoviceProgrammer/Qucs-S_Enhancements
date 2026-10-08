"""check_schematic with a diagram failing its limit; a Bode diagram's panes: a count, a trace's pane, a limit on its phase pane."""
import json, mcp
from common import rc, show
s = mcp.Server('p49')
p, sim = rc(s)
d = s.call('add_diagram', {'type': 'rect', 'traces': ['tran.v(in)'], 'limits': [{'upper': 0.9, 'label': 'max'}]})
show('verdict', d.get('verdict'), 200)
c = s.call('check_schematic', {})
show('check_schematic', [w for w in c.get('warnings', []) if 'limit' in json.dumps(w).lower() or 'diagram' in json.dumps(w).lower() or 'max' in json.dumps(w)], 500)
b = s.call('add_diagram', {'type': 'bode', 'traces': ['ac.v(out)']})['diagram']
for label, tool, args in (('bode panes 3', 'edit_diagram', {'diagram': b, 'panes': 3}), ('bode panes 1', 'edit_diagram', {'diagram': b, 'panes': 1}),
                          ('bode trace into pane 2', 'add_trace', {'diagram': b, 'variable': 'ac.v(in)', 'pane': 2}),
                          ('bode limit on pane 2 (phase)', 'edit_diagram', {'diagram': b, 'limits': [{'lower': -90, 'pane': 2}]}),
                          ('bode limit pane 1 in dB', 'edit_diagram', {'diagram': b, 'limits': [{'upper': -20, 'pane': 1}]})):
    r = s.call(tool, args)
    show(f'[err={int(s.last_error)}] {label}', r if s.last_error else {k: r.get(k) for k in ('panes', 'traces', 'limits', 'verdict') if k in r}, 500)
s.close()

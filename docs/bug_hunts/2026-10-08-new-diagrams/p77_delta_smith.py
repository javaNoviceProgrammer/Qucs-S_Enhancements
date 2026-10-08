"""A delta marker on a Smith chart and a polar diagram (complex values): what Δy is, against the values by hand."""
import shutil, mcp
from common import EX, show
s = mcp.Server('p77')
s.call('new_project', {'name': 'sp'}); d = s.ws + '/sp_prj'
shutil.copy(EX + '/templates_ngspice/S-parameter_active_analysis.sch', d + '/sp.sch')
s.call('open_project', {'name': 'sp'}); s.call('open_document', {'path': d + '/sp.sch'})
s.call('simulate', {'simulator': 'ngspice'})
for t in ('smith', 'polar'):
    r = s.call('add_diagram', {'type': t, 'traces': ['ac.s_1_1']})
    a = s.call('add_marker', {'diagram': r['diagram'], 'at': 5e6}); b = s.call('add_marker', {'diagram': r['diagram'], 'at': 15e6, 'relative_to': 1})
    show(f'{t}: m1', {k: a.get(k) for k in ('value', 'text')}, 200)
    show(f'{t}: m2', {k: b.get(k) for k in ('value', 'delta', 'text')}, 400)
s.close()

"""Smith-chart circles on the S-parameter template (ngspice): odd frequencies and levels, other chart types; get_dataset's stability."""
import json, shutil, sys, mcp
from common import EX, show
s = mcp.Server('p11')
s.call('new_project', {'name': 'sp'}); d = s.ws + '/sp_prj'
shutil.copy(EX + '/templates_ngspice/S-parameter_active_analysis.sch', d + '/sp.sch')
s.call('open_project', {'name': 'sp'}); s.call('open_document', {'path': d + '/sp.sch'})
sim = s.call('simulate', {'simulator': 'ngspice'})
show('sim', sim.get('succeeded') if isinstance(sim, dict) else sim, 100)
names = [v['name'] for v in s.call('get_dataset', {'points': 0}).get('variables', [])]
print('vars', names[:40])
def c(label, args, n=500):
    r = s.call('add_diagram', dict(type='smith', traces=['ac.s_1_1'], **args))
    o = r.get('smith_circles', r) if isinstance(r, dict) else r
    show(f'[{s.last_time:4.1f}s err={int(s.last_error)}] {label}:', o, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
ALL = [{'kind': 'stability_in'}, {'kind': 'stability_out'}, {'kind': 'gain', 'level': 10}, {'kind': 'noise', 'level': 2}]
c('all four, the middle', {'smith_circles': {'circles': ALL}}, 900)
for f in (1e15, 0, -5, 1, 'abc', '10 MHz', 'nan', 1e-300):
    c(f'frequency {f!r}', {'smith_circles': {'circles': ALL, 'frequency': f}})
for k, lv in (('gain', 1000), ('gain', -1000), ('gain', 0), ('noise', 0), ('noise', -3), ('noise', 1000), ('gain', None), ('noise', None)):
    cc = {'kind': k} if lv is None else {'kind': k, 'level': lv}
    c(f'{k} level {lv}', {'smith_circles': {'circles': [cc]}})
c('a circle twice, 40 of them', {'smith_circles': {'circles': [{'kind': 'gain', 'level': i / 4} for i in range(40)]}}, 300)
c('no circles', {'smith_circles': {'circles': []}})
c('a kind unknown', {'smith_circles': {'circles': [{'kind': 'nyquist'}]}})
for t2 in ('polar_smith', 'smith_polar', 'polar', 'rect'):
    r = s.call('add_diagram', {'type': t2, 'traces': ['ac.s_1_1'], 'smith_circles': {'circles': ALL}})
    show(f'{t2} with circles err={int(s.last_error)}', r.get('smith_circles', r) if isinstance(r, dict) else r, 300)
r = s.call('add_diagram', {'type': 'admittance_smith', 'traces': ['ac.s_2_2'], 'smith_circles': {'circles': ALL}})
show('admittance', r.get('smith_circles', r) if isinstance(r, dict) else r, 600)
g = s.call('get_dataset', {'variables': ['ac.s_2_1'], 'measure': ['stability'], 'at': [1e15, -1, 0]})
show('stability at odd x', g, 1500)
s.call('export_image', {'diagram': 3, 'save_as': s.root + '/circles.png'})
print('alive', s.p.poll() is None, s.root)
s.close()

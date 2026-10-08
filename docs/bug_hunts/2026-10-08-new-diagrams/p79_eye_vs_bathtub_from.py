"""A4 again with both measured from 2 ns (the example's eye diagram's 'from'): the eye's width at BER 1e-12 against the bathtub's opening."""
import shutil, mcp
from common import EX, show
s = mcp.Server('p79')
s.call('new_project', {'name': 'eye'}); d = s.ws + '/eye_prj'
shutil.copy(EX + '/ngspice/NGspice features/PRBS_eye_diagram.sch', d + '/eye.sch')
s.call('open_project', {'name': 'eye'}); s.call('open_document', {'path': d + '/eye.sch'})
s.call('simulate', {'simulator': 'ngspice'})
g = s.call('get_dataset', {'variables': ['tran.v(rx)'], 'measure': ['eye'], 'offset': 2e-9})
e = g['variables'][0]['measurements']['eye']
print('eye from 2 ns:', {k: e.get(k) for k in ('width at BER 1e-12', 'width, UI', 'jitter, rms', 'jitter, peak to peak')})
r = s.call('add_diagram', {'type': 'bathtub', 'traces': ['tran.v(rx)'], 'bathtub': {'from': 2e-9}})
b = r['analyses'][0]
print('bathtub from 2 ns:', {k: b.get(k) for k in ('opening, UI', 'random jitter, rms', 'deterministic jitter', 'total jitter')})
s.close()

"""The .SENS example with R2 renamed R_load: is it among the tornado chart's parts?"""
import shutil, mcp
from common import EX, show
s = mcp.Server('p19')
s.call('new_project', {'name': 'se'}); d = s.ws + '/se_prj'
t = open(EX + '/ngspice/NGspice features/sensitivityACandDC.sch').read().replace('<R R2 1 ', '<R R_load 1 ')
open(d + '/sens.sch', 'w').write(t)
s.call('open_project', {'name': 'se'}); s.call('open_document', {'path': d + '/sens.sch'})
sim = s.call('simulate', {'simulator': 'ngspice'})
names = sorted({v['name'] for v in s.call('get_dataset', {'points': 0})['variables'] if '.' not in v['name'] and ('load' in v['name'] or v['name'] in ('r1', 'v1'))})
print('dataset has', names)
r = s.call('add_diagram', {'type': 'tornado', 'traces': ['r1_scale'], 'tornado': {'parts': True}})
show('tornado parts', r.get('tornado', r) if isinstance(r, dict) else r, 900)
s.close()

# The hint of an imported dataset that has a variable: each trace with no data reads every imported dataset in full.
import os, sys, json, shutil, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_slow'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
shutil.copy(HERE + '/big.dat', ws + '/scope.dat')   # a big dataset from elsewhere (47 MB)
s = Server(ws)
s.call('new_document', {}); s.call('save_document', {'as': 'p.sch'})
for name in ('a', 'b', 'c'):
    t = time.time(); s.call('import_data', {'file': 'scope.dat', 'name': 'big_' + name}); print('import', name, round(time.time() - t, 2), 's')
t = time.time(); r = s.call('add_diagram', {'traces': ['x%d' % i for i in range(8)]}); print('add_diagram of 8 traces not there:', round(time.time() - t, 2), 's')
t = time.time(); s.call('get_schematic', {}); print('get_schematic:', round(time.time() - t, 2), 's')
t = time.time(); s.call('reload_data', {}); print('reload_data:', round(time.time() - t, 2), 's')
s.close()

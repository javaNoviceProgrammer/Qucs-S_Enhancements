# The trace names get_dataset gives an imported dataset's variables (name:variable), handed back to the tools that take a variable.
import os, sys, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
ws = HERE + '/ws_tn'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
s = Server(ws)
def c(tool, args):
    try: r = s.call(tool, args)
    except RuntimeError as e: return 'ERROR ' + str(e)[:260]
    return r if isinstance(r, str) else json.dumps(r)[:260]
open(ws + '/m.csv', 'w').write('f,gain\n' + ''.join('%g,%g\n' % (10 ** (i / 10), 1 / (1 + (10 ** (i / 10) / 1e3) ** 2) ** .5) for i in range(61)))
c('new_document', {}); c('save_document', {'as': 'p.sch'})
print(c('import_data', {'file': 'm.csv'})[:120])
print('get_dataset variables [m:gain]:', c('get_dataset', {'path': 'm.dat', 'variables': ['m:gain'], 'measure': ['bandwidth']}))
print('get_dataset variables [gain]:', c('get_dataset', {'path': 'm.dat', 'variables': ['gain'], 'measure': ['bandwidth']}))
print('add_diagram:', c('add_diagram', {'traces': ['m:gain'], 'x_axis': {'log': True}}))
print('add_marker peak:', c('add_marker', {'diagram': 1, 'trace': 1, 'at': '-3dB'}))
print('get_dataset path p.sch, variables [m:gain]:', c('get_dataset', {'variables': ['m:gain']}))
s.close()

# import_data of a large CSV (500,000 rows, 5 columns): time, and the server's memory.
import os, sys, json, shutil, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import Server, HERE
open(HERE + '/settings/qucs/qucs_s.ini', 'w').write('[General]\nDefaultSimulator=1\n')
ws = HERE + '/ws_bigimp'; shutil.rmtree(ws, ignore_errors=True); os.makedirs(ws)
with open(ws + '/big.csv', 'w') as f:
    f.write('t,a,b,c,d\n')
    f.write(''.join('%.9e,%.6f,%.6f,%.6f,%.6f\n' % (i * 1e-9, (i % 1000) * 1e-3, 1 + (i % 7), 2 + (i % 11), 3 + (i % 13)) for i in range(500000)))
print('csv MB', round(os.path.getsize(ws + '/big.csv') / 1e6, 1))
s = Server(ws)
s.call('new_document', {}); s.call('save_document', {'as': 'p.sch'})
s.call('add_diagram', {'traces': ['big:a', 'big:b']}, ok=False)
t = time.time(); r = s.call('import_data', {'file': 'big.csv'}, ok=False); dt = time.time() - t
rss = subprocess.run(['ps', '-o', 'rss=', '-p', str(s.p.pid)], capture_output=True, text=True).stdout.strip()
print('import_data:', round(dt, 2), 's; server RSS now', int(rss) // 1024 if rss else '?', 'MB;', json.dumps(r.get('diagrams') if isinstance(r, dict) else r)[:200])
t = time.time(); s.call('import_data', {'name': 'big', 'reload': True}, ok=False); print('reload:', round(time.time() - t, 2), 's')
s.close()

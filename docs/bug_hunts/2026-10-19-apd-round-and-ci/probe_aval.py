import sys, time, re
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/close/wa')
path = H + '/close/aval_on.qfem'
c.call('open_document', {'path': path})
t = time.time(); err, text = c.call('fem_solve', {'path': path, 'timeout': 600}); a = time.time() - t
print('solve', round(a, 1), 'ERR' if err else 'ok', text[:300].replace('\n', ' '))
err, d = c.call('fem_describe', {'path': path}); m = re.search(r'"stopped":"([^"]*)"', d)
print('stopped:', m.group(1) if m else None)
err, v = c.call('fem_evaluate', {'path': path, 'expression': 'semi.Mion', 'all': True})
print('Mion', v[:600])
c.close()

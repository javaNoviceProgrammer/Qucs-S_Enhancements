import sys, time, re
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/close/ws')
path = H + '/close/boron.qfem'
c.call('open_document', {'path': path})
err, text = c.call('fem_solve', {'path': path, 'timeout': 3})
err2, d = c.call('fem_describe', {'path': path}); m = re.search(r'"at work":"([^"]*)"', d)
t = time.time(); err3, text3 = c.call('fem_stop', {'path': path}); b = time.time() - t
print('at work:', m.group(1) if m else None, '| fem_stop', round(b, 1), text3[-120:])
c.close()

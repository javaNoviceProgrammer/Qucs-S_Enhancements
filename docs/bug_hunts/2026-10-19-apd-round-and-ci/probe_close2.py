import sys, time, re
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
app = sys.argv[3] if len(sys.argv) > 3 else None
for wait in [3]:
    c = Client(H + '/close/wb' + str(wait), app=app) if app else Client(H + '/close/wb' + str(wait))
    path = H + '/close/boron.qfem'
    c.call('open_document', {'path': path})
    t = time.time(); err, text = c.call('fem_solve', {'path': path, 'timeout': wait}); a = time.time() - t
    err2, d = c.call('fem_describe', {'path': path})
    m = re.search(r'"at work":"([^"]*)"', d)
    t = time.time(); err3, text3 = c.call('close_document', {'path': path, 'unsaved': 'discard'}); b = time.time() - t
    print('wait', wait, 'solve', round(a, 1), text[:100].replace('\n', ' '), '| at work:', (m.group(1)[:160] if m else None), '| close', round(b, 1), text3[:60])
    c.close()

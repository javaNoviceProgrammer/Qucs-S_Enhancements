import sys, json, time
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
path = H + '/mcp/apd_slow.qfem'
c = Client(H + '/mcp/w1')
def show(tag, r):
    err, text = r
    print('==', tag, 'ERR' if err else 'ok', '|', text[:900].replace('\n', ' / '))
show('open', c.call('fem_model', {'action': 'open', 'path': path}))
err, d = c.call('fem_describe', {'path': path})
import re
studies = re.findall(r'"tag":\s*"(std[^"]*|study[^"]*)"', d)
print('studies', studies[:5])
study = studies[0] if studies else 'study1'
t = time.time(); show('solve 3s', c.call('fem_solve', {'path': path, 'study': study, 'timeout': 3})); print('took', round(time.time()-t, 1))
show('describe at work', c.call('fem_describe', {'path': path}))
show('stop', c.call('fem_stop', {'path': path}))
time.sleep(1)
show('describe after stop', c.call('fem_describe', {'path': path}))
show('evaluate', c.call('fem_evaluate', {'path': path, 'study': study, 'expression': 'semi.I_k', 'all': True}))
t = time.time(); show('solve again 4s', c.call('fem_solve', {'path': path, 'study': study, 'timeout': 4})); print('took', round(time.time()-t, 1))
show('stop2', c.call('fem_stop', {'path': path}))
time.sleep(1)
show('describe 2', c.call('fem_describe', {'path': path}))
c.close()

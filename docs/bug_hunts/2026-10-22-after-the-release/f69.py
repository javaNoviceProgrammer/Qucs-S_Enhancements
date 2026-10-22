# depth 2 against 3 for the long word, and pictures of the window at depth 2 and 4 (a short word run)
import sys, time, os, base64, json
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w69')
def opened(body, shot=None):
    p = D + '/e.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    if shot:
        e2, r2 = c.call('screenshot', {'path': p, 'area': 'window'})
        print('shot', e2, str(r2)[:300])
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for depth in (2, 3):
    print('depth %d, 6400 x xsqrt: %.2f s' % (depth, opened('>' * depth + ' ' + 'xsqrt' * 6400 + '\n')), flush=True)
opened('>' * 4 + ' ' + 'xsqrt' * 40 + '\n', shot=True)
c.close()

# the long word's time against the quoting depth: does it level off where the text's width runs out?
import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w68')
def timed(body):
    p = D + '/d.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    if body.count('xsqrt') == 400:
        e2, r2 = c.call('screenshot', {'save_as': D + '/d%d.png' % body.index(' ')})
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for depth in (4, 8, 12, 16, 24):
    print('depth %2d, 6400 x xsqrt: %.2f s' % (depth, timed('>' * depth + ' ' + 'xsqrt' * 6400 + '\n')), flush=True)
for depth in (4, 16):
    timed('>' * depth + ' ' + 'xsqrt' * 400 + '\n')
c.close()

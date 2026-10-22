# d1030's line 24: nested blockquotes over a line of inline math - depth and length
import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w66')
def timed(body):
    p = D + '/q.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for depth, n, unit in ((50, 1600, '$\\sqrt'), (50, 3200, '$\\sqrt'), (50, 6400, '$\\sqrt'),
                       (12, 6400, '$\\sqrt'), (25, 6400, '$\\sqrt'), (100, 6400, '$\\sqrt'),
                       (50, 6400, 'xsqrt'), (50, 6400, 'x y '), (50, 6400, '*a* ')):
    body = '>' * depth + ' ' + unit * n + ('$' if unit.startswith('$') else '') + '\n'
    print('depth %3d, %5d x %-7r (%6d chars): %.2f s' % (depth, n, unit, len(body), timed(body)), flush=True)
c.close()

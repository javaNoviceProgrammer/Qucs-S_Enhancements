import sys, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w6')
def timed(body):
    p = D + '/c.md'
    open(p, 'w').write(body)
    t = time.time(); c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for d, spans, w in ((200, 87, 300), (200, 175, 300), (100, 175, 300), (50, 175, 300), (200, 175, 10), (200, 350, 10), (200, 700, 10), (0, 700, 10)):
    body = '- ' * d + 'deep $$\n> $' + ('{' * w + '$$') * spans
    print('depth %3d, %4d spans of %3d braces (%6d chars): %.2f s' % (d, spans, w, len(body), timed(body)))
c.close()

import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w4')
def timed(name, body):
    p = D + '/' + name + '.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for d, L, tail in ((200, 26000, ' $$'), (200, 52000, ' $$'), (100, 52000, ' $$'), (50, 52000, ' $$'), (200, 52000, ''), (200, 52000, ' x'), (0, 52000, ' $$')):
    body = '- ' * d + 'deep' + tail + '\n> $' + '{' * L
    print('depth %3d, %6d braces, tail %-4r: %.2f s' % (d, L, tail, timed('s', body)))
c.close()

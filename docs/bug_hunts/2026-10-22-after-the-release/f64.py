# d1030: a long line of inline math spans ("$\sqrt$" repeated), with and without a blockquote or list
import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
os.makedirs(D, exist_ok=True)
c = Client(D + '/w64')
def timed(name, body):
    p = D + '/' + name + '.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for pre, n in (('> ', 1000), ('> ', 2000), ('> ', 4000), ('', 1000), ('', 2000), ('', 4000), ('- ', 2000), ('> ', 4000)):
    body = 'x\n\n' + pre + '$\\sqrt$' * n + '\n'
    print('%-3r %5d spans: %.2f s' % (pre, n, timed('i', body)), flush=True)
c.close()

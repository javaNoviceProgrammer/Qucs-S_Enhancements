import sys, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w12')
def timed(body):
    p = D + '/f63.md'
    open(p, 'w').write(body)
    t = time.time(); c.call('open_document', {'path': p}); a = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return a
for n in (25, 50, 100, 200):
    print('a display formula of %3d image links: %.2f s' % (n, timed('$$\n' + '![a](a.png)' * n + '\n$$')))
for n in (50, 100, 200):
    print('a display formula of %3d `x`: %.2f s' % (n, timed('$$\n' + '`x` ' * n + '\n$$')))
for n in (100, 200):
    print('inline $...$ of %3d links: %.2f s' % (n, timed('$' + '[l](b)' * n + '$')))
c.close()

# a long word without a break (no math): by depth of quoting, and in a .txt
import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w67')
def timed(body, ext='.md'):
    p = D + '/w' + ext
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for pre, n, ext in (('', 3200, '.md'), ('', 6400, '.md'), ('> ', 6400, '.md'), ('>> ', 6400, '.md'), ('- ', 6400, '.md'),
                    ('    ', 6400, '.md'), ('', 6400, '.txt'), ('', 12800, '.md')):
    body = pre + 'xsqrt' * n + '\n'
    print('%-6r %5d x xsqrt %s (%6d chars): %.2f s' % (pre, n, ext, len(body), timed(body, ext)), flush=True)
c.close()

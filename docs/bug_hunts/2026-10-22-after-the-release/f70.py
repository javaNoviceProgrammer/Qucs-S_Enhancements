# a long run of letters and digits (no break opportunity) at no depth: a paragraph (base64-like text), a fenced code block, a table cell
import sys, time, os, random
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w70')
random.seed(1)
def blob(n): return ''.join(random.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789') for _ in range(n))
def opened(body):
    p = D + '/f.md'
    open(p, 'w').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
for n in (32000, 64000, 128000):
    b = blob(n)
    print('%6d chars: paragraph %.2f s, code block %.2f s, table cell %.2f s' % (
        n, opened('text\n\n' + b + '\n'), opened('```\n' + b + '\n```\n'), opened('| a |\n|---|\n| ' + b + ' |\n')), flush=True)
c.close()

# d1030 bisected: which lines make the 8.5 s
import sys, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
src = open(S + '/hunt2/fzmdr/d1030.md', encoding='utf-8', errors='surrogateescape').read().split('\n')
c = Client(D + '/w65')
def timed(body):
    p = D + '/b.md'
    open(p, 'w', encoding='utf-8', errors='surrogateescape').write(body)
    t = time.time(); e, r = c.call('open_document', {'path': p}); dt = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return dt
def lines(a, b): return '\n'.join(src[a - 1:b]) + '\n'
L24 = src[23]
cases = [('all', '\n'.join(src)), ('24', L24 + '\n'), ('1-24', lines(1, 24)), ('22-24', lines(22, 24)),
         ('24-55', lines(24, 55)), ('24 quotes only', '> ' + '>' * 50 + ' ' + '$\\sqrt' * 6450 + '$\n'),
         ('24 one quote', '> ' + '$\\sqrt' * 6450 + '$\n')]
for name, body in cases:
    print('%-16s %6d chars: %.2f s' % (name, len(body), timed(body)), flush=True)
c.close()

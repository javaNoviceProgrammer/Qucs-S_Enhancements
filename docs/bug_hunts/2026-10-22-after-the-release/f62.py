import sys, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
L = open(D + '/d697.md', encoding='utf-8', errors='surrogatepass').read().split('\n')
c = Client(D + '/w11')
def timed(body):
    p = D + '/bis2.md'
    open(p, 'w', encoding='utf-8', errors='surrogatepass').write(body)
    t = time.time(); c.call('open_document', {'path': p}); a = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return a
for name, idx in (('23-29', range(23, 30)), ('25-29', range(25, 30)), ('27-29', range(27, 30)), ('23-28', range(23, 29)), ('25-28', range(25, 29)), ('22-29 no 29', [22, 23, 24, 25, 26, 27, 28])):
    print(name, '%.2f s' % timed('\n'.join(L[i] for i in idx)))
print('synthetic: "    $$ $$x$$" then "    $"+{*2881 then "    `code"+img*100: %.2f s' % timed('    $$ $$x$$\n    $' + '{' * 2881 + '\n    `code ' + '![a](a.png)' * 100))
print('synthetic without images: %.2f s' % timed('    $$ $$x$$\n    $' + '{' * 2881 + '\n    `code '))
print('synthetic, images only: %.2f s' % timed('    `code ' + '![a](a.png)' * 100))
c.close()

import sys, time
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
lines = open(D + '/d697.md', encoding='utf-8', errors='surrogatepass').read().split('\n')
c = Client(D + '/w10')
def timed(body):
    p = D + '/bis.md'
    open(p, 'w', encoding='utf-8', errors='surrogatepass').write(body)
    t = time.time(); c.call('open_document', {'path': p}); a = time.time() - t
    c.call('close_document', {'path': p, 'unsaved': 'discard'})
    return a
lo, hi = 0, len(lines)
print('all', len(lines), 'lines %.2f s' % timed('\n'.join(lines)))
# shrink from the end, then from the start, while it stays slow
while hi - lo > 1:
    mid = (lo + hi) // 2
    if timed('\n'.join(lines[lo:mid])) > 1.5: hi = mid; continue
    if timed('\n'.join(lines[mid:hi])) > 1.5: lo = mid; continue
    break
print('kept lines', lo, hi, '%.2f s' % timed('\n'.join(lines[lo:hi])))
for i in range(lo, hi): print(i, len(lines[i]), repr(lines[i][:150]))
c.close()

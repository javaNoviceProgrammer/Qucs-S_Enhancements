import sys, json, time, base64
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/diag'
c = Client(D + '/w2')
p = D + '/amp.sch'; dpl = p
c.call('open_document', {'path': p}); c.call('simulate', {'path': p, 'timeout': 120})
c.call('open_document', {'path': dpl})
e, t = c.call('get_schematic', {'path': dpl}); print('dpl', e, t[:600])
for d in (1, 2, 3, 4):
    for tr in (1, 2, 3, 4):
        e, t = c.call('edit_trace', {'path': dpl, 'diagram': d, 'trace': tr, 'hidden': True})
        print(d, tr, e, t[:160])
e, t = c.call('screenshot', {'path': dpl, 'area': 'paper'}); print('shot', e, t[:200])
e, t = c.call('export_image', {'path': dpl, 'save_as': D + '/hidden.png'}); print('export', e, t[:200])
c.close()

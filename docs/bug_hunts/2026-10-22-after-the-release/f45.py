import sys, json, time, os
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/kfr'
c = Client(D + '/w3')
p = D + '/flow.qfem'
for name, args in (('open_document', {'path': p}), ('fem_build', {'path': p, 'timeout': 1}), ('fem_stop', {'path': p}), ('fem_describe', {'path': p}), ('fem_stop', {'path': p})):
    t = time.time(); e, r = c.call(name, args); print(name, e, '%.2f s' % (time.time() - t), r[:170].replace('\n', ' '))
c.close()

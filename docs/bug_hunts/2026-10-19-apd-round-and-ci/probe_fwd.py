import sys
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/mcp/w-f')
p = H + '/mcp/fwd.qfem'
c.call('open_document', {'path': p})
c.call('fem_solve', {'path': p, 'timeout': 300})
for e in ['semi.Mion', 'semi.Iion', 'semi.Igen', 'semi.I_k']:
    err, v = c.json('fem_evaluate', {'path': p, 'expression': e, 'all': True})
    print(e, [(x['name'].split(':')[0], x['value']) for x in v['values']] if isinstance(v, dict) and 'values' in v else v)
c.close()

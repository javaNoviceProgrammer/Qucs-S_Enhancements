import sys
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/close/wa2')
path = H + '/close/' + sys.argv[3]
c.call('open_document', {'path': path})
err, text = c.call('fem_solve', {'path': path, 'timeout': 600})
for e in ['semi.In', 'semi.Ip']:
    err, v = c.json('fem_evaluate', {'path': path, 'expression': e, 'all': True})
    print(e, [(x['name'].split(':')[0], round(x['SI'], 4) if x['SI'] is not None else None) for x in v['values']] if isinstance(v, dict) else v)
c.close()

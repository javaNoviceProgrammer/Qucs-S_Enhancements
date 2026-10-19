import sys, re
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
for name in ['light_sweep', 'light_1e26', 'light_1e28', 'light_1e30']:
    c = Client(H + '/mcp/w-l')
    p = H + '/mcp/' + name + '.qfem'
    c.call('open_document', {'path': p})
    err, t = c.call('fem_solve', {'path': p, 'timeout': 300})
    err2, v = c.json('fem_evaluate', {'path': p, 'expression': 'semi.I_k', 'all': True})
    e3, d = c.call('fem_describe', {'path': p}); st = re.findall(r'"stopped":"([^"]*)"', d)
    vals = [(x['name'].split(':')[0], x['value']) for x in v['values']] if isinstance(v, dict) and 'values' in v else v
    print(name, 'ERR ' + t[:200] if err else 'ok', vals, st)
    c.close()

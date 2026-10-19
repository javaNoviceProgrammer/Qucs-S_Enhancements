import sys
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
for d in ['field', 'fermi']:
    c = Client(H + '/mcp/w-g')
    p = H + '/mcp/g_' + d + '.qfem'
    c.call('open_document', {'path': p})
    c.call('fem_solve', {'path': p, 'timeout': 120})
    err, t = c.call('fem_plot', {'path': p, 'expression': 'semi.Gion'})
    err2, v = c.call('fem_evaluate', {'path': p, 'expression': 'semi.Iion'})
    print(d, t[:160], '| Iion', v[:160])
    c.close()

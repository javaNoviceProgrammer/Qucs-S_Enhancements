import sys, re, time
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/mcp/w-plot')
path = H + '/mcp/apd_slow.qfem'
c.call('open_document', {'path': path})
c.call('fem_solve', {'path': path, 'timeout': 3})
c.call('fem_stop', {'path': path})
time.sleep(1)
for args in [{'expression': 'semi.Gion'}, {'expression': 'semi.V', 'solution': 3}, {'expression': 'semi.V', 'solution': 50}, {'expression': 'semi.I_k'}]:
    a = dict(path=path, **args)
    err, text = c.call('fem_plot', a)
    print(args, 'ERR' if err else 'ok', text[:220].replace('\n', ' '))
c.close()

import sys, time
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
c = Client(H + '/mcp/w-idle')
path = H + '/mcp/apd_slow.qfem'
print('open', c.call('open_document', {'path': path})[1][:80])
print('stop idle', c.call('fem_stop', {'path': path}))
print('solve 2s', c.call('fem_solve', {'path': path, 'timeout': 2})[1][:120])
print('stop 1', c.call('fem_stop', {'path': path})[1][-160:])
print('stop 2', c.call('fem_stop', {'path': path}))
print('solve 0', c.call('fem_solve', {'path': path, 'timeout': 0})[1][:160])
print('stop 3', c.call('fem_stop', {'path': path})[1][-120:])
print('solve -5', c.call('fem_solve', {'path': path, 'timeout': -5})[1][:160])
print('stop 4', c.call('fem_stop', {'path': path})[1][-120:])
c.close()

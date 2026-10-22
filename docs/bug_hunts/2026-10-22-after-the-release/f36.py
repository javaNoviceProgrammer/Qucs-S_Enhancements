import sys, time, subprocess, threading
S = '/tmp/qucs-hunt'
sys.path.insert(0, S)
from mcp_client import Client
D = S + '/hunt2/mdhang'
c = Client(D + '/w7')
body = '- ' * 200 + 'deep $$\n> $' + ('{' * 300 + '$$') * 175
open(D + '/slow.md', 'w').write(body)
def sampler():
    time.sleep(1.5)
    subprocess.run(['sample', str(c.p.pid), '3', '-file', D + '/slow_sample.txt'], capture_output=True)
th = threading.Thread(target=sampler); th.start()
t = time.time(); c.call('open_document', {'path': D + '/slow.md'}); print('open %.2f s' % (time.time() - t))
th.join(); c.close()

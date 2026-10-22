import sys, json, time, os, shutil
S = '/tmp/qucs-hunt'
sys.path.insert(0, S); sys.path.insert(0, S + '/hunt2')
from mcp_client import Client
from probe import H
shutil.copy(H + '/kfr/flow.qfem', H + '/f54flow.qfem'); path = H + '/f54flow.qfem'
for wait_call in (False, True):
    c = Client(H + '/f54.w')
    c.call('open_document', {'path': path})
    if wait_call:
        c._send({'jsonrpc': '2.0', 'id': 501, 'method': 'tools/call', 'params': {'name': 'fem_build', 'arguments': {'path': path, 'what': 'mesh', 'timeout': 300}}})
    else:
        c.call('fem_build', {'path': path, 'what': 'mesh', 'timeout': 1})
    time.sleep(1)
    t = time.time(); c.p.stdin.close()
    try: rc = c.p.wait(timeout=300)
    except Exception: c.p.kill(); rc = 'killed'
    print('a fem_build %s: exit %s %.1f s after the client went' % ('waiting' if wait_call else 'answered (timeout 1)', rc, time.time() - t))

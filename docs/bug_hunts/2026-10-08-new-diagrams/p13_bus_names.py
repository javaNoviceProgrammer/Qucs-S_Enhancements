"""Buses (Insert > Bus, add_painting bus) with odd names: what members they list, and Check Schematic, timed."""
import sys, mcp
from common import rc, show
s = mcp.Server('p13')
p, sim = rc(s)
def c(label, tool, args, n=300):
    r = s.call(tool, args)
    show(f'[{s.last_time:5.1f}s err={int(s.last_error)}] {label}:', r, n)
    if s.p.poll() is not None: print('SERVER DIED', open(s.root + '/server.err').read()[-3000:]); sys.exit(1)
    return r
y = 400
for name in ('D[7:0]', 'D[0:7]', 'D[2147483647:0]', 'D[-2147483648:2147483647]', 'D[100000:0]', 'D[-5:0]', '[3:0]', 'D[a:b]', 'D[7:0', 'D[7:0][3:0]', 'A,B,C', 'D[3:0],E[1:0]', '', 'gnd[1:0]', 'in[1:0]', 'out'):
    c(f'bus {name!r}', 'add_painting', {'type': 'bus', 'name': name, 'from': [400, y], 'to': [600, y]})
    y += 20
c('check_schematic', 'check_schematic', {}, 1500)
c('netlist', 'get_netlist', {}, 300)
c('save', 'save_document', {}, 120)
c('close', 'close_document', {}, 100)
c('reopen', 'open_document', {'path': p}, 120)
print('alive', s.p.poll() is None)
s.close()

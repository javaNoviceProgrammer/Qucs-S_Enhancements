import json, os, sys, random, time, shutil
import mcp
from mcp import Server, HERE
mcp.APP = sys.argv[1]; random.seed(int(sys.argv[2])); budget = float(sys.argv[3])
WS = HERE + '/wsnf'; shutil.rmtree(WS, ignore_errors=True); os.makedirs(WS)
nodes = ['0', 'in', 'out', 'a', 'b', 'n1', 'n2', 'vcc', 'vee', 'gnd', 'x.y', '1', 'OUT', 'Out']
vals = ['1k', '10u', '1meg', '0', '-1', '{r}', '1e308', 'abc', '100n', '5']
def netlist():
    lines = [random.choice(['title', 'amp', '* comment', 'R1 a b 1k'])]
    for i in range(random.randint(1, 14)):
        k = random.choice('RCLVIEDQMX')
        n = random.sample(nodes, 4)
        if k in 'RCL': lines.append('%s%d %s %s %s' % (k, i, n[0], n[1], random.choice(vals)))
        elif k == 'V': lines.append('V%d %s %s %s' % (i, n[0], n[1], random.choice(['DC 5', 'AC 1', 'DC 0 AC 1', 'PULSE(0 5 0 1n 1n 1u 2u)', 'SIN(0 1 1k)', ''])))
        elif k == 'I': lines.append('I%d %s %s DC 1m' % (i, n[0], n[1]))
        elif k == 'E': lines.append('E%d %s %s %s %s %s' % (i, n[0], n[1], n[2], n[3], random.choice(vals)))
        elif k == 'D': lines.append('D%d %s %s DMOD' % (i, n[0], n[1]))
        elif k == 'Q': lines.append('Q%d %s %s %s QMOD' % (i, n[0], n[1], n[2]))
        elif k == 'M': lines.append('M%d %s %s %s %s MMOD' % (i, n[0], n[1], n[2], n[3]))
        else: lines.append('X%d %s %s %s sub' % (i, n[0], n[1], n[2]))
    if random.random() < 0.5: lines += ['.subckt sub p q r', 'R1 p q 1k', 'C1 q r 1n', '.ends']
    if random.random() < 0.5: lines += ['.model DMOD D', '.model QMOD NPN(BF=100)', '.model MMOD NMOS']
    lines.append(random.choice(['.op', '.tran 1n 1u', '.ac dec 10 1 1meg', '']))
    lines.append('.end')
    return '\n'.join(lines)
s = Server(WS); t0 = time.time(); n = 0; deaths = []
while time.time() - t0 < budget:
    text = netlist(); n += 1
    title = random.choice([None, '', 'x' * 400, '"q"', 'a\nb', 'é'])
    args = {'text': text}
    if title is not None: args['title'] = title
    try:
        r = s.call('import_netlist', args, ok=False)
        s.call('arrange', {'wire_labels': True, 'feedback': random.choice(['below', 'above']), 'straighten': True}, ok=False)
        s.call('check_schematic', {'subcircuits': True}, ok=False)
        s.call('get_netlist', {'map': True}, ok=False)
        s.call('close_document', {'unsaved': 'discard'}, ok=False)
    except Exception as e:
        deaths.append((text.replace('\n', ' | ')[:200], repr(e)[:80]))
        try: s.close()
        except Exception: pass
        s = Server(WS)
try: s.close()
except Exception: pass
print(n, 'netlists,', len(deaths), 'server deaths')
for d in deaths[:6]: print(d)

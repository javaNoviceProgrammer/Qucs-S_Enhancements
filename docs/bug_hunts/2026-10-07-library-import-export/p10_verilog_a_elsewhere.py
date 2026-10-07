"""p10: a library with its Verilog-A embedded, made in one workspace and imported into another (another computer): placed and simulated there."""
import os, re, json, shutil, mcp, lib
VA = ('`include "disciplines.vams"\n`include "consts.vh"\nmodule vres(p, n);\n  inout p, n;\n  electrical p, n;\n  parameter real r = 1k from (0:inf);\n'
      '  analog I(p, n) <+ V(p, n) / r;\nendmodule\n')
CONSTS = '`define MYPI 3.14159\n'
SUB = ('<Qucs Schematic 26.1.6>\n<Components>\n  <Port P1 1 220 100 -23 12 0 0 "1" 1 "analog" 0>\n  <Port P2 1 280 100 4 12 1 2 "2" 1 "analog" 0>\n'
       '  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 ".model m1 vres r=1k" 1 "N1 a b m1" 0 "" 0 "" 0 "" 0>\n'
       '  <R R2 1 280 130 15 -26 0 1 "3k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n  <GND * 1 280 160 0 0 0 0>\n</Components>\n'
       '<Wires>\n  <220 100 220 100 "a" 250 70 0 "">\n  <280 100 280 100 "b" 310 70 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
if __name__ == '__main__':
    a = mcp.Server('p10a')
    d = lib.project(a, 'src', {'vres.va': VA, 'consts.vh': CONSTS, 'vdiv.sch': SUB})
    lib.show('  vdiv.sch itself (want 0.75):', lib.bench(a, {'type': 'Sub', 'properties': {'File': 'vdiv.sch'}}, d + '/direct.sch'))
    r = a.call('create_library', {'name': 'VaLib', 'destination': 'project'})
    print('create:', a.last_error, r.get('models'), [m for m in r.get('messages', []) if 'mbed' in m or 'arn' in m or 'rror' in m])
    carried = a.root + '/usb'
    os.makedirs(carried)
    shutil.copy(d + '/VaLib.lib', carried)
    shutil.copytree(d + '/VaLib', carried + '/VaLib')
    a.close()
    b = mcp.Server('p10b')
    db = lib.project(b, 'use', {})
    r = b.call('import_library', {'path': carried + '/VaLib.lib'})
    print('import:', b.last_error, [os.path.relpath(w, b.ws) for w in r.get('written', [])])
    out = lib.bench(b, r['parts'][0]['place'], db + '/use.sch')
    print('  imported part (want 0.75):', json.dumps(out)[:800].replace(b.root, '<root>'))
    print('  project now:', sorted(os.path.relpath(os.path.join(x, f), db) for x, _, fs in os.walk(db) for f in fs if not f.startswith('use.')))
    b.close()

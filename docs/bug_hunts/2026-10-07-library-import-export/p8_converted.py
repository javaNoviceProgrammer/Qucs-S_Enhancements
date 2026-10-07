"""p8: a SPICE subcircuit file made a Qucs library by the converter (Tools > Convert Data File, output Qucs library: qucsconv_rf -of qucslib), imported and simulated."""
import os, re, json, subprocess, mcp, lib
s = mcp.Server('p8')
src = s.root + '/vend.cir'
open(src, 'w').write('* vendor parts\n.subckt VDIV 1 2\nR1 1 2 1k\nR2 2 0 3k\n.ends VDIV\n'
                     '.subckt VRC in out\nR1 in out 1k\nC1 out 0 1n\nR2 out 0 1k\n.ends VRC\n')
src2 = s.root + '/models.cir'
open(src2, 'w').write('* vendor models\n.model D1N4148 D(IS=2.52n RS=0.568 N=1.752 BV=100 IBV=100u CJO=4p)\n'
                      '.model Q2N3904 NPN(IS=6.734f BF=416.4 VAF=74.03)\n.model MYNMOS NMOS(LEVEL=1 VTO=0.7 KP=110u)\n.end\n')
p = subprocess.run([mcp.REPO + '/build/qucsator_rf/src/converter/qucsconv_rf', '-if', 'spice', '-of', 'qucslib', '-ln', 'Models', '-i', src2, '-o', s.root + '/Models.lib'], capture_output=True, text=True)
print('models converter:', p.returncode, (p.stdout + p.stderr)[:300])
print(open(s.root + '/Models.lib').read()[:1500])
conv = mcp.REPO + '/build/qucsator_rf/src/converter/qucsconv_rf'
out = s.root + '/Vend.lib'
p = subprocess.run([conv, '-if', 'spice', '-of', 'qucslib', '-ln', 'Vend', '-i', src, '-o', out], capture_output=True, text=True)
print('converter:', p.returncode, (p.stdout + p.stderr)[:400])
text = open(out).read() if os.path.isfile(out) else ''
print('sections:', sorted(set(re.findall(r'\n\s*<(\w+)>', text))), 'components:', re.findall(r'<Component (\S+)>', text))
print(text[:900])
d = lib.project(s, 'cv', {})
r = s.call('import_library', {'path': s.root + '/Models.lib'})
print('import:', s.last_error, json.dumps(r)[:500].replace(s.root, '<root>'))
for simulator in ['ngspice', 'qucsator']:
    s.call('set_simulator', {'simulator': simulator})
    for part in (r.get('parts', []) if isinstance(r, dict) else []):
        s.call('new_document', {})
        a = s.call('add_component', {'type': part['place']['type'], 'name': 'X1', 'x': 320, 'y': 120, 'properties': part['place']['properties']})
        print(f"  {simulator} {part['part']} placed:", s.last_error, str(a)[:200])
        if not s.last_error:
            n = s.call('get_netlist', {})
            print('    netlist:', [l for l in str(n).split('\n') if l[:1] in 'DQMdqm.' and 'X1' in l or l.lower().startswith('.model')][:4])
            c = s.call('check_schematic', {})
            print('    check:', json.dumps(c)[:300])
s.close()

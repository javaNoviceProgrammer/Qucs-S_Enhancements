"""p4: import_library - a SPICE library including files beside it, a Qucs-S library with CRLF lines (a Windows colleague's), each part simulated."""
import os, re, json, mcp, lib
s = mcp.Server('p4')
elsewhere = s.root + '/elsewhere'
os.makedirs(elsewhere + '/sub', exist_ok=True)
# (a) A vendor's SPICE library: its subcircuit uses one defined in a file it includes, and a .lib section of another.
open(elsewhere + '/vend.lib', 'w').write(
    '* vendor parts\n.include "sub/inner.inc"\n.lib "corners.lib" TT\n'
    '.subckt VDIV 1 2\nXA 1 2 INNER\n.ends VDIV\n'
    '.subckt VSEC 1 2\nXB 1 2 CORNER\n.ends VSEC\n')
open(elsewhere + '/sub/inner.inc', 'w').write('.subckt INNER 1 2\nR1 1 2 1k\nR2 2 0 1k\n.ends INNER\n')
open(elsewhere + '/corners.lib', 'w').write('.lib TT\n.subckt CORNER 1 2\nR1 1 2 1k\nR2 2 0 3k\n.ends CORNER\n.endl TT\n')
d = lib.project(s, 'imp', {'div.sch': lib.divider('1k', '3k')})
# The library where it is, placed as a SpLib: what the parts should give.
for dev, want in [('VDIV', 0.5), ('VSEC', 0.75)]:
    lib.show(f'  {dev} from where it is (want {want}):', lib.bench(s, {'type': 'SpLib', 'properties': {'File': elsewhere + '/vend.lib', 'Device': dev}}, d + f'/where_{dev}.sch'))
r = s.call('import_library', {'path': elsewhere + '/vend.lib'})
lib.show('import vend:', {'error': s.last_error, 'kind': r.get('kind'), 'written': [os.path.basename(w) for w in r.get('written', [])],
                          'note': r.get('note')} if isinstance(r, dict) else r)
if isinstance(r, dict):
    for p, want in zip(r.get('parts', []), [0.5, 0.75]):
        lib.show(f"  imported {p['part']} (want {want}):", lib.bench(s, p['place'], d + f"/imp_{p['part']}.sch"))
# (b) A Qucs-S library written with CRLF line ends.
r = s.call('create_library', {'name': 'Crlf', 'destination': 'project'})
src = d + '/Crlf.lib'
crlf = open(src, newline='').read().replace('\r\n', '\n').replace('\n', '\r\n')
os.makedirs(elsewhere + '/win', exist_ok=True)
open(elsewhere + '/win/WinLib.lib', 'w', newline='').write(crlf.replace('"Crlf"', '"WinLib"').replace('Crlf_', 'WinLib_'))
os.remove(src)
r = s.call('import_library', {'path': elsewhere + '/win/WinLib.lib'})
lib.show('import WinLib (CRLF):', {'error': s.last_error, 'kind': r.get('kind'), 'parts': r.get('parts')} if isinstance(r, dict) else r)
dp = s.call('describe_part', {'library': 'WinLib', 'part': 'div'})
lib.show('  describe_part WinLib div:', dp)
if isinstance(r, dict) and r.get('parts'):
    lib.show('  WinLib div (want 0.75):', lib.bench(s, r['parts'][0]['place'], d + '/imp_win.sch'))
s.close()

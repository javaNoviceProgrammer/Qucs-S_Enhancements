"""p13: a library whose .va is itself a link into another folder (a folder of links: git-annex, Nix, stow), the .va
including a file of its real folder: linked into a project, compiled, simulated; the include changed: rebuilt?"""
import os, shutil, time, libsetup
s, team, proj, made, saved = libsetup.setup('p13')
openvaf = shutil.which('openvaf-r') or shutil.which('openvaf')
s.call('set_settings', {'scope': 'app', 'values': {'Locations/OpenVAF Path': openvaf}})
real = s.root + '/store'
os.makedirs(real, exist_ok=True)
open(real + '/vres_params.vams', 'w').write('`define VRES_R 2k\n')
open(real + '/vres.va', 'w').write(libsetup.VA.replace('`include "disciplines.vams"\n', '`include "disciplines.vams"\n`include "vres_params.vams"\n')
                                   .replace('parameter real r = 1k', 'parameter real r = `VRES_R'))
os.remove(team + '/VaRes/vres.va')
os.symlink(real + '/vres.va', team + '/VaRes/vres.va')       # the library's source: a link into the store
# a test bench: 1 V through R2 1k into the part (its model r from the include): v(out)
s.call('batch', {'calls': [
    {'tool': 'add_component', 'arguments': {'path': proj + '/top.sch', 'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}}},
    {'tool': 'add_component', 'arguments': {'path': proj + '/top.sch', 'type': 'R', 'name': 'R2', 'x': 220, 'y': 100, 'properties': {'R': '1k'}}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'V1.1', 'to': 'R2.1'}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'R2.2', 'to': 'X1.1'}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'X1.2', 'to': 'ground'}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'V1.2', 'to': 'ground'}},
    {'tool': 'set_label', 'arguments': {'path': proj + '/top.sch', 'at': 'R2.2', 'name': 'out'}},
    {'tool': 'add_component', 'arguments': {'path': proj + '/top.sch', 'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}}]})
s.call('save_document', {'path': proj + '/top.sch'})
def vout():
    sim = s.call('simulate', {'path': 'top.sch', 'brief': True})
    op = s.call('get_dataset', {'path': 'top.sch', 'operating_point': True})
    osdi = proj + '/Libraries/VaRes/vres.osdi'
    return sim.get('succeeded'), op.get('operating point', {}).get('nodes', {}).get('v(out)') if isinstance(op, dict) else op, \
        os.path.getmtime(osdi) if os.path.exists(osdi) else None, sim.get('errors')
a = vout(); print('first run: ok, v(out), osdi mtime:', a[:3], a[3])
time.sleep(1.2)
open(real + '/vres_params.vams', 'w').write('`define VRES_R 1k\n')    # the include changed: r = 1k, v(out) = 0.5
b = vout(); print('include changed: ok, v(out), osdi mtime:', b[:3], '| rebuilt:', b[2] != a[2], '| expected v(out) 0.5')
s.close()

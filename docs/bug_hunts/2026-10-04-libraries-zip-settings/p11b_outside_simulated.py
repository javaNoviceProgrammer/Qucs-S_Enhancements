"""p11b: simulate a schematic outside the open project that places the library's part: links made in the project?"""
import os, shutil, libsetup
s, team, proj, made, saved = libsetup.setup('p11b')
link = proj + '/Libraries/VaRes/vres.va'
openvaf = shutil.which('openvaf-r') or shutil.which('openvaf')
if openvaf: s.call('set_settings', {'scope': 'app', 'values': {'Locations/OpenVAF Path': openvaf}})
other = s.ws + '/elsewhere'
os.makedirs(other, exist_ok=True)
txt = open(proj + '/top.sch').read()
open(other + '/x.sch', 'w').write(txt)
s.call('delete', {'path': proj + '/top.sch', 'names': ['X1']})
s.call('save_document', {'path': proj + '/top.sch'})
print('before: link in project', os.path.islink(link))
s.call('open_document', {'path': other + '/x.sch'})
s.call('batch', {'calls': [
    {'tool': 'add_component', 'arguments': {'path': other + '/x.sch', 'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}}},
    {'tool': 'connect', 'arguments': {'path': other + '/x.sch', 'from': 'V1.1', 'to': 'X1.1'}},
    {'tool': 'connect', 'arguments': {'path': other + '/x.sch', 'from': 'X1.2', 'to': 'ground'}},
    {'tool': 'connect', 'arguments': {'path': other + '/x.sch', 'from': 'V1.2', 'to': 'ground'}},
    {'tool': 'add_component', 'arguments': {'path': other + '/x.sch', 'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}}]})
s.call('save_document', {'path': other + '/x.sch'})
sim = s.call('simulate', {'path': other + '/x.sch', 'brief': True})
print('simulated outside x.sch: ok', sim.get('succeeded') if isinstance(sim, dict) else sim, '| openvaf', bool(openvaf))
print('after: link in the project', os.path.islink(link), '| project Libraries:', os.listdir(proj + '/Libraries') if os.path.isdir(proj + '/Libraries') else None)
print('team folder:', sorted(os.listdir(team + '/VaRes')))
r = s.call('save_document', {'path': proj + '/top.sch'})
print('top saved again:', str(r)[-200:], '| link', os.path.islink(link))
s.close()

"""A library .va whose includes include each other (a.vams <-> b.vams), and one including itself: the source scan
(osdiselection sourceIncludes) ends, the run says OpenVAF's error, nothing hangs; then fixed: compiles."""
import os, shutil, time, json, libsetup
s, team, proj, made, saved = libsetup.setup('p38')
openvaf = shutil.which('openvaf-r') or shutil.which('openvaf')
s.call('set_settings', {'scope': 'app', 'values': {'Locations/OpenVAF Path': openvaf}})
lib = team + '/VaRes'
open(lib + '/a.vams', 'w').write('`include "b.vams"\n`define A 1\n')
open(lib + '/b.vams', 'w').write('`include "a.vams"\n`include "b.vams"\n`define B 1\n')
t = open(lib + '/vres.va').read()
open(lib + '/vres.va', 'w').write(t.replace('`include "disciplines.vams"\n', '`include "disciplines.vams"\n`include "a.vams"\n'))
s.call('batch', {'calls': [
    {'tool': 'add_component', 'arguments': {'path': proj + '/top.sch', 'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'V1.1', 'to': 'X1.1'}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'X1.2', 'to': 'ground'}},
    {'tool': 'connect', 'arguments': {'path': proj + '/top.sch', 'from': 'V1.2', 'to': 'ground'}},
    {'tool': 'add_component', 'arguments': {'path': proj + '/top.sch', 'type': '.DC', 'name': 'DC1', 'x': 120, 'y': 400}}]})
print('save:', str(s.call('save_document', {'path': proj + '/top.sch'}))[:200])
t0 = time.time(); sim = s.call('simulate', {'path': 'top.sch', 'brief': True, 'timeout': 90})
print('cycle: %.1fs succeeded=%s errors=%s' % (time.time() - t0, sim.get('succeeded') if isinstance(sim, dict) else '-', json.dumps(sim.get('errors') if isinstance(sim, dict) else sim)[:400]))
print('links in project:', sorted(os.listdir(proj + '/Libraries/VaRes')) if os.path.isdir(proj + '/Libraries/VaRes') else None)
open(lib + '/a.vams', 'w').write('`define A 1\n'); open(lib + '/b.vams', 'w').write('`define B 1\n')
t0 = time.time(); sim = s.call('simulate', {'path': 'top.sch', 'brief': True, 'timeout': 90})
print('fixed: %.1fs succeeded=%s errors=%s' % (time.time() - t0, sim.get('succeeded') if isinstance(sim, dict) else '-', json.dumps(sim.get('errors') if isinstance(sim, dict) else sim)[:300]))
s.close()

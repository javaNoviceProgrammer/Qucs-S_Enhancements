"""A search path library VaRes (vres.va) placed in a project's schematic, saved: Libraries/VaRes/vres.va linked."""
import os, mcp
VA = ('`include "disciplines.vams"\nmodule vres(p, n);\n  inout p, n;\n  electrical p, n;\n  parameter real r = 1k from (0:inf);\n'
      '  analog I(p, n) <+ V(p, n) / r;\nendmodule\n')
SUB = ('<Qucs Schematic 26.1.5>\n<Components>\n  <Port P1 1 220 100 -23 12 0 0 "1" 1 "analog" 0>\n  <Port P2 1 280 100 4 12 1 2 "2" 1 "analog" 0>\n'
       '  <SpiceModel SpiceModel1 1 120 300 -27 16 0 0 ".model m1 vres r=2k" 1 "N1 a b m1" 0 "" 0 "" 0 "" 0>\n</Components>\n'
       '<Wires>\n  <220 100 220 100 "a" 250 70 0 "">\n  <280 100 280 100 "b" 310 70 0 "">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
def setup(name, team_name='VaRes'):
    s = mcp.Server(name)
    team = s.root + '/team'
    os.makedirs(team, exist_ok=True)
    s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [team]}})
    s.call('new_project', {'name': 'src'})
    src = s.ws + '/src_prj'
    open(src + '/vres.va', 'w').write(VA)
    open(src + '/vres.sch', 'w').write(SUB)
    s.call('open_project', {'name': 'src'})
    made = s.call('create_library', {'name': team_name, 'subcircuits': ['vres'], 'destination': team})
    s.call('new_project', {'name': 'use'})
    proj = s.ws + '/use_prj'
    s.call('open_project', {'name': 'use'})
    place = s.call('describe_part', {'library': team_name, 'part': 'vres'})['place']
    s.call('new_document', {})
    s.call('add_component', {'type': place['type'], 'name': 'X1', 'x': 340, 'y': 200, 'properties': place['properties']})
    saved = s.call('save_document', {'as': proj + '/top.sch'})
    return s, team, proj, made, saved

"""p12: (CheckCommands on) is a .control block with a shell line in an imported library part's SPICE text found by Check Schematic? Detection only:
the line is the bare keyword (no command), the schematic has no analysis, and nothing is simulated - check_schematic and get_netlist only."""
import os, re, json, mcp, lib
# With the check on (Application Settings: commands looked for; off by default).
s = mcp.Server('p12', settings_ini=f'[General]\nCheckCommands=true\nQucsator={mcp.REPO}/build/qucsator_rf/src/qucsator_rf\n')
d = lib.project(s, 'cmd', {'div.sch': lib.divider('1k', '3k')})
r = s.call('create_library', {'name': 'Plain', 'destination': 'project'})
text = open(d + '/Plain.lib').read()
os.makedirs(s.root + '/colleague', exist_ok=True)
# The colleague's copy: a control block after the subcircuit, in its <Spice> section; and one in a file it attaches.
text = text.replace('.ENDS\n  </Spice>', '.ENDS\n.control\nshell\n.endc\n  </Spice>').replace('"Plain"', '"Theirs"').replace('Plain_', 'Theirs_')
text = text.replace('  </Spice>\n', '  </Spice>\n<SpiceAttach "extra.lib">\n', 1)
open(s.root + '/colleague/Theirs.lib', 'w').write(text)
os.makedirs(s.root + '/colleague/Theirs', exist_ok=True)
open(s.root + '/colleague/Theirs/extra.lib', 'w').write('* extra\n.control\nshell\n.endc\n')
os.remove(d + '/Plain.lib')
r = s.call('import_library', {'path': s.root + '/colleague/Theirs.lib'})
print('import:', s.last_error, r.get('kind') if isinstance(r, dict) else r, r.get('note') if isinstance(r, dict) else '')
s.call('new_document', {})
s.call('add_component', {'type': 'Vdc', 'name': 'V1', 'x': 100, 'y': 200, 'properties': {'U': '1 V'}})
s.call('add_component', {'type': 'Lib', 'name': 'X1', 'x': 320, 'y': 120, 'properties': {'Lib': 'Theirs', 'Comp': 'div'}})
s.call('connect', {'from': 'V1.1', 'to': 'X1.1'})
s.call('connect', {'from': 'V1.2', 'to': 'ground'})
s.call('save_document', {'as': d + '/no_analysis.sch'})     # no analysis: nothing would run
c = s.call('check_schematic', {})
print('check_schematic warnings:', json.dumps(c.get('warnings') if isinstance(c, dict) else c)[:600])
n = s.call('get_netlist', {})
print('netlist lines with control or shell:', [l for l in str(n).split('\n') if re.search(r'(?i)\.control|\.endc|^\s*shell|INCLUDE', l)])
print('commands said by check_schematic:', [w['message'] for w in c.get('warnings', []) if 'command' in w['message']] if isinstance(c, dict) else c)
s.close()

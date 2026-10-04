"""p21: create_library of subcircuits whose file names hold > , a quote, a space: the library's parts placed and loaded?"""
import mcp, json, os, libsetup
s = mcp.Server('p21')
s.call('new_project', {'name': 'src'})
src = s.ws + '/src_prj'
names = ['amp>x', 'qu"ote', 'two words', 'lt<gt']
for n in names: open(f'{src}/{n}.sch', 'w').write(libsetup.SUB.replace('SpiceModel1 1 120 300 -27 16 0 0 ".model m1 vres r=2k" 1 "N1 a b m1" 0 "" 0 "" 0 "" 0', 'R R1 1 250 100 -26 15 0 0 "1 kOhm" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0'))
s.call('open_project', {'name': 'src'})
r = s.call('create_library', {'name': 'OddNames', 'subcircuits': [n + '.sch' for n in names]})
print('create:', s.last_error, json.dumps([p.get('part') for p in r.get('parts', [])] if isinstance(r, dict) else r)[:300], str(r.get('messages', ''))[:200] if isinstance(r, dict) else '')
lib = s.ws + '/user_lib/OddNames.lib'
print('components in the file:', [l for l in open(lib).read().split('\n') if l.startswith('<Component')] if os.path.isfile(lib) else None)
for p in (r.get('parts', []) if isinstance(r, dict) else []):
    s.call('new_document', {})
    s.call('add_component', {'type': p['place']['type'], 'name': 'U1', 'x': 200, 'y': 200, 'properties': p['place']['properties']})
    g = s.call('get_schematic', {'components': ['U1']})
    print('  ', repr(p['part']), 'pins', len(g['components'][0].get('pins', [])) if isinstance(g, dict) and g.get('components') else g)
    s.call('close_document', {'unsaved': 'discard'})
s.close()

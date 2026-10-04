"""p17: library files on a search path named with a quote, a space, a non-ASCII letter, a semicolon: each part placed as
list_libraries says, saved, reopened: still the part (its pins)?"""
import os, json, mcp
s = mcp.Server('p17')
src = open(os.environ['MYLIB']).read()
names = ['quo"te', 'two words', 'Bibliothèque', 'semi;colon', 'brack<et>', 'lead-dash']
os.makedirs(s.root + '/libs', exist_ok=True)
for n in names: open(f'{s.root}/libs/{n}.lib', 'w').write(src)
s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [s.root + '/libs']}})
for i, n in enumerate(names):
    l = s.call('list_libraries', {'library': f'{s.root}/libs/{n}.lib'})
    if not isinstance(l, dict) or not l.get('parts'):
        print(n, 'not listed:', str(l)[:150]); continue
    place = l['parts'][0]['place']
    s.call('new_document', {})
    r = s.call('add_component', {'type': place['type'], 'name': 'U1', 'x': 200, 'y': 200, 'properties': place['properties']})
    p1 = s.call('get_schematic', {'components': ['U1']})
    pins1 = len(p1['components'][0].get('pins', [])) if isinstance(p1, dict) and p1.get('components') else p1
    f = f'{s.ws}/t{i}.sch'
    s.call('save_document', {'as': f})
    s.call('close_document', {'path': f})
    s.call('open_document', {'path': f})
    p2 = s.call('get_schematic', {'path': f, 'components': ['U1']})
    pins2 = len(p2['components'][0].get('pins', [])) if isinstance(p2, dict) and p2.get('components') else str(p2)[:120]
    line = [x for x in open(f).read().split('\n') if '<Lib U1' in x]
    print(f'{n!r:16} Lib={place["properties"]["Lib"][-24:]!r:28} pins placed {pins1} reopened {pins2} | {line[0][:90] if line else "no line"}')
s.close()

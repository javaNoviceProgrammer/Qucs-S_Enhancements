"""p3b: a subcircuit placing a library part, made a library of another name (nothing replaced), and under replace; each part simulated."""
import os, re, json, mcp, lib
s = mcp.Server('p3b')
d = lib.project(s, 'rep', {'div.sch': lib.divider('1k', '3k')})
r = s.call('create_library', {'name': 'Keep', 'subcircuits': ['div']})
place = r['parts'][0]['place']
s.call('new_document', {})
s.call('add_component', {'type': 'Port', 'name': 'P1', 'x': 100, 'y': 100})
s.call('add_component', {'type': 'Port', 'name': 'P2', 'x': 600, 'y': 100})
s.call('add_component', {'type': place['type'], 'name': 'X1', 'x': 320, 'y': 120, 'properties': place['properties']})
s.call('connect', {'from': 'P1.1', 'to': 'X1.1'})
s.call('connect', {'from': 'X1.2', 'to': 'P2.1'})
s.call('save_document', {'as': d + '/wrap.sch'})
s.call('close_document', {})

def model(libfile, comp):
    text = open(libfile).read()
    m = re.search(r'\n<Component ' + comp + r'>\n(.*?)\n</Component>', text, re.S)
    body = m.group(1) if m else ''
    spice = re.search(r'<Spice>(.*?)</Spice>', body, re.S)
    return {'spice lines': len(spice.group(1).strip().split('\n')) if spice and spice.group(1).strip() else 0,
            'symbol lines': len(re.findall(r'\n    <', body))}

r = s.call('create_library', {'name': 'Other', 'subcircuits': ['wrap']})
print('Other:', s.last_error, [m for m in r.get('messages', []) if 'WARN' in m or 'rror' in m], model(s.ws + '/user_lib/Other.lib', 'wrap'))
lib.show('  Other:wrap (want 0.75):', lib.bench(s, r['parts'][0]['place'], d + '/use_other.sch'))
r = s.call('create_library', {'name': 'Keep', 'replace': True})
print('Keep replaced:', s.last_error, [m for m in r.get('messages', []) if 'WARN' in m or 'rror' in m], model(s.ws + '/user_lib/Keep.lib', 'wrap'))
for p in r.get('parts', []):
    lib.show(f"  Keep:{p['part']} (want 0.75):", lib.bench(s, p['place'], d + f"/use_keep_{p['part']}.sch"))
s.close()

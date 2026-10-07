"""p1: create_library of subcircuits with a dot, a folder, a leading digit, a space or an accent in their names; each part simulated."""
import os, re, json, mcp, lib
s = mcp.Server('p1')
names = ['div.sch', 'div.v2.sch', 'sub/deep.sch', '2stage.sch', 'my div.sch', 'Résumé.sch']
d = lib.project(s, 'odd', {n: lib.divider('1k', '3k') for n in names})
r = s.call('create_library', {'name': 'Odd'})
lib.show('create:', {'error': s.last_error, 'parts': [p.get('part') for p in r.get('parts', [])] if isinstance(r, dict) else r,
                     'messages': r.get('messages') if isinstance(r, dict) else None})
text = open(s.ws + '/user_lib/Odd.lib').read() if os.path.isfile(s.ws + '/user_lib/Odd.lib') else ''
print('components:', re.findall(r'\n<Component ([^\n]*)>', text))
print('subckts:', re.findall(r'(?i)\n\s*\.subckt\s+(\S+)', text))
if isinstance(r, dict):
    for p in r.get('parts', []):
        out = lib.bench(s, p['place'], d + f"/use_{re.sub(r'[^A-Za-z0-9]', '_', p['part'])}.sch")
        lib.show(f"  {p['part']!r} place={json.dumps(p['place']['properties'])}:", out)
s.close()

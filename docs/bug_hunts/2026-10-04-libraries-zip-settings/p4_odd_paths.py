"""p4: library search paths that are a file, unreadable, gone, a duplicate, user_lib itself, relative."""
import os, json, mcp, shutil
s = mcp.Server('p4')
r = s.root
open(r + '/afile', 'w').write('x')
os.makedirs(r + '/locked', exist_ok=True); open(r + '/locked/L.lib', 'w').write('<Qucs Library 26.1.5 "L">\n'); os.chmod(r + '/locked', 0)
os.makedirs(r + '/gone', exist_ok=True)
os.makedirs(r + '/ok', exist_ok=True)
shutil.copy(os.environ['MYLIB'], r + '/ok/mylib.lib')
paths = [r + '/afile', r + '/locked', r + '/gone', r + '/ok', r + '/ok/', r + '/ok/../ok', s.ws + '/user_lib', 'relative/dir', '']
res = s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': paths}})
print('set_settings:', s.last_error, str(res)[:400])
shutil.rmtree(r + '/gone')
g = s.call('get_settings', {'scope': 'app', 'keys': ['Locations/Library search paths']})
print('get_settings:', json.dumps(g)[:600])
l = s.call('list_libraries', {})
for sec in l.get('sections', []): print('  section', sec.get('section'), sec.get('folder'), 'missing' if sec.get('missing') else '', [x.get('name') for x in sec.get('libraries', [])][:5], [x.get('unreadable') for x in sec.get('libraries', []) if x.get('unreadable')])
f = s.call('find_library_component', {'query': 'opamp'})
print('find opamp:', s.last_error, str(f)[:300])
os.chmod(r + '/locked', 0o755)
s.close()

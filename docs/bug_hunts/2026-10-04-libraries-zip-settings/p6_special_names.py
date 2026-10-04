"""p6: create_library into the project named after its special folders: Scratch (emptied by Clean Scratch), Libraries."""
import os, json, mcp, libsetup
s = mcp.Server('p6')
s.call('new_project', {'name': 'src'})
src = s.ws + '/src_prj'
open(src + '/vres.va', 'w').write(libsetup.VA)
open(src + '/vres.sch', 'w').write(libsetup.SUB)
s.call('open_project', {'name': 'src'})
for name in ('Scratch', 'Libraries'):
    r = s.call('create_library', {'name': name, 'subcircuits': ['vres'], 'destination': 'project'})
    print(name, ': error', s.last_error, '| models', r.get('models') if isinstance(r, dict) else str(r)[:200])
print('Scratch holds:', sorted(os.listdir(src + '/Scratch')) if os.path.isdir(src + '/Scratch') else None)
c = s.call('clean_scratch', {})
print('clean_scratch:', s.last_error, str(c)[:300])
print('Scratch holds after:', sorted(os.listdir(src + '/Scratch')) if os.path.isdir(src + '/Scratch') else None,
      '| Scratch.lib still:', os.path.isfile(src + '/Scratch.lib'))
print('Libraries holds:', sorted(os.listdir(src + '/Libraries')) if os.path.isdir(src + '/Libraries') else None)
s.close()

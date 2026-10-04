"""p1: a record in Libraries/<x>/ edited by hand (or brought by a clone) whose item path climbs out with
x/../..: does the sync, the library unused, take away a file outside the folder?"""
import json, os, mcp
s = mcp.Server('p1b')
proj = s.ws + '/evil_prj'
os.makedirs(proj + '/Libraries/Evil', exist_ok=True)
open(proj + '/victim.txt', 'w').write('the user\'s file\n')
open(proj + '/victim.osdi', 'w').write('x')
os.makedirs(proj + '/keep', exist_ok=True)
open(proj + '/keep/notes.txt', 'w').write('notes\n')
open(proj + '/Libraries/.qucs-libraries', 'w').write('mark\n')
json.dump({'library': 'Evil', 'folder': '/nowhere/Evil', 'created': True,
           'files': [{'path': './../../victim.txt', 'original': '/nowhere/Evil/x.va', 'kind': 'copy'},
                     {'path': './../../keep/notes.txt', 'original': '/nowhere/Evil/y.va', 'kind': 'copy'}]},
          open(proj + '/Libraries/Evil/.qucs-library.json', 'w'))
open(proj + '/top.sch', 'w').write('<Qucs Schematic 26.1.5>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
r = s.call('open_project', {'name': 'evil'})
print('open_project:', str(r)[:300])
for f in ('victim.txt', 'victim.osdi', 'keep/notes.txt', 'Libraries'):
    print(f, 'exists' if os.path.exists(proj + '/' + f) else 'GONE')
s.close()

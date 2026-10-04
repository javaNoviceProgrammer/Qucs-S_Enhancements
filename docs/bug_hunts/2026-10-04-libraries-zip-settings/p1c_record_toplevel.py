"""p1c: F1 through the migration of a top-level folder (a record from before Libraries/): ./../ in its paths."""
import json, os, mcp
s = mcp.Server('p1c')
proj = s.ws + '/old_prj'
os.makedirs(proj + '/OldLib', exist_ok=True); os.makedirs(proj + '/docs', exist_ok=True)
open(proj + '/docs/thesis.tex', 'w').write('years of work\n')
json.dump({'library': 'OldLib', 'folder': '/nowhere/OldLib', 'created': True,
           'files': [{'path': './../docs/thesis.tex', 'original': '/nowhere/OldLib/x.va', 'kind': 'copy'}]},
          open(proj + '/OldLib/.qucs-library.json', 'w'))
open(proj + '/top.sch', 'w').write('<Qucs Schematic 26.1.5>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
s.call('open_project', {'name': 'old'})
print('docs/thesis.tex:', 'exists' if os.path.exists(proj + '/docs/thesis.tex') else 'GONE', '| OldLib:', os.path.exists(proj + '/OldLib'))
s.close()

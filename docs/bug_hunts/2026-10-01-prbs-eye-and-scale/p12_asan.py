import sys, json, os
sys.path.insert(0, '/private/tmp/claude-501/-Users-meisam-git-Qucs-S-Enhancements/ff1f864c-ed6a-4c31-a788-5998e059b547/scratchpad/repro')
from client import Server
root = sys.argv[1]; os.makedirs(root + '/ws/p', exist_ok=True)
APP = '/Users/meisam/git/Qucs-S_Enhancements/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s'
os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
s = Server(root, '/Users/meisam/git/Qucs-S_Enhancements', APP)
def call(tool, args={}):
    e, t = s.call(tool, args)
    if e: print('  ', tool, json.dumps(args)[:80], '->', t[0][:140].replace('\n', ' '))
    return e, t[0]
sub2 = ('<Qucs Schematic 26.1.5>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n'
        '  <Port P1 1 100 100 -23 12 0 0 "1" 1 "analog" 0>\n  <Port P2 1 300 100 -23 12 0 0 "2" 1 "analog" 0>\n'
        '  <R R1 1 200 100 15 -26 0 0 "1k" 1>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n')
open(root + '/ws/p/sub.sch', 'w').write(sub2)
call('new_document', {'kind': 'schematic'})
call('save_document', {'as': root + '/ws/p/top.sch'})
call('add_component', {'type': 'MUTX', 'name': 'Tr1', 'x': 100, 'y': 100})
for n in ('5', '1', '3', '2'): call('edit_component', {'name': 'Tr1', 'properties': {'coils': n}})
call('add_component', {'type': 'EDD', 'name': 'D1', 'x': 400, 'y': 100})
for n in ('4', '1', '3'): call('edit_component', {'name': 'D1', 'properties': {'Branches': n}})
call('add_component', {'type': 'RFEDD', 'name': 'RF1', 'x': 700, 'y': 100})
for n in ('4', '1', '3'): call('edit_component', {'name': 'RF1', 'properties': {'Ports': n}})
call('add_component', {'type': 'Sub', 'name': 'SUB1', 'x': 100, 'y': 400, 'properties': {'File': 'sub.sch'}})
sub3 = sub2.replace('</Components>', '  <Port P3 1 400 100 -23 12 0 0 "3" 1 "analog" 0>\n</Components>')
open(root + '/ws/p/sub.sch', 'w').write(sub3)
call('edit_component', {'name': 'SUB1', 'properties': {'File': 'sub.sch'}})
call('reload_data', {})
call('add_component', {'type': 'Lib', 'name': 'D2', 'x': 400, 'y': 400, 'properties': {'Lib': 'Diodes', 'Comp': '1N4148'}})
for k in range(6): call('undo', {})
for k in range(6): call('redo', {})
call('replace_component', {'name': 'Tr1', 'type': 'R'})
call('replace_component', {'name': 'D1', 'type': 'C'})
call('save_document', {})
call('close_document', {'unsaved': 'discard'})
call('open_document', {'path': root + '/ws/p/top.sch'})
e, t = call('get_schematic', {'format': 'summary'})
print('parts after reopen:', [c.get('name') for c in json.loads(t).get('components', [])])
call('close_document', {'unsaved': 'discard'})
s.close()
err = open(root + '/server.err').read()
print('server exit ok; sanitizer reports:', err.count('AddressSanitizer') + err.count('runtime error:'))
print('\n'.join(l for l in err.splitlines() if 'Sanitizer' in l or 'runtime error' in l)[:1500])

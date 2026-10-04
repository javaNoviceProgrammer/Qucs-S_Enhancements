"""p7: a part naming its library in another case (macOS: the file system ignores case): vares for VaRes."""
import os, json, libsetup
s, team, proj, made, saved = libsetup.setup('p7')
print('VaRes saved:', sorted(os.listdir(proj + '/Libraries')))
txt = open(proj + '/top.sch').read()
open(proj + '/lower.sch', 'w').write(txt.replace('"VaRes"', '"vares"'))
open(proj + '/top.sch', 'w').write(txt.replace('<Lib X1', '<GND G9').split('<Lib')[0] if False else txt)
s.call('open_document', {'path': proj + '/lower.sch'})
st = s.call('get_schematic', {'path': 'lower.sch', 'components': ['X1']})
print('lower X1 pins:', len(st['components'][0].get('pins', [])) if isinstance(st, dict) else st)
r = s.call('save_document', {'path': 'lower.sch'})
print('save lower:', str(r)[-200:])
print('Libraries now:', sorted(os.listdir(proj + '/Libraries')), [json.load(open(f'{proj}/Libraries/{d}/.qucs-library.json'))['library'] for d in os.listdir(proj + '/Libraries') if os.path.isdir(f'{proj}/Libraries/{d}')])
# the VaRes schematic loses its part: only the lower one uses it now
txt2 = open(proj + '/top.sch').read()
s.call('close_document', {'path': 'top.sch', 'unsaved': 'discard'})
s.call('delete', {'path': 'lower.sch', 'names': ['X1']}) if False else None
os.remove(proj + '/top.sch')
r = s.call('save_document', {'path': 'lower.sch'})
print('top.sch gone, save lower:', str(r)[-200:])
print('Libraries now:', sorted(os.listdir(proj + '/Libraries')) if os.path.isdir(proj + '/Libraries') else None)
c = s.call('check_schematic', {'path': 'lower.sch'})
print('check:', str(c)[:300])
s.close()

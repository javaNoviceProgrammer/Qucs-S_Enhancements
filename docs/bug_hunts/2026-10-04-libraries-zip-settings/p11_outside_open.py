"""p11: a schematic outside the open project, open (unsaved changes or not), placing the library's part: does the
project keep its link for it? Its own project gets none?"""
import os, libsetup
s, team, proj, made, saved = libsetup.setup('p11')
link = proj + '/Libraries/VaRes/vres.va'
other = s.ws + '/elsewhere'
os.makedirs(other, exist_ok=True)
open(other + '/x.sch', 'w').write(open(proj + '/top.sch').read())
s.call('open_document', {'path': other + '/x.sch'})
# top.sch loses the part and is saved: only the outside schematic uses it now
s.call('delete', {'path': proj + '/top.sch', 'names': ['X1']})
r = s.call('save_document', {'path': proj + '/top.sch'})
print('top.sch saved without the part:', str(r)[-160:])
print('  link in the project:', os.path.islink(link), '| Libraries in elsewhere:', os.path.exists(other + '/Libraries'))
s.call('edit_component', {'path': other + '/x.sch', 'name': 'X1', 'properties': {}}) if False else None
r = s.call('save_document', {'path': other + '/x.sch'})
print('outside x.sch saved:', str(r)[-200:])
print('  link in the project:', os.path.islink(link), '| Libraries in elsewhere:', os.path.exists(other + '/Libraries'))
s.call('close_document', {'path': other + '/x.sch'})
r = s.call('save_document', {'path': proj + '/top.sch'})
print('outside closed, top saved:', '| link:', os.path.islink(link))
s.close()

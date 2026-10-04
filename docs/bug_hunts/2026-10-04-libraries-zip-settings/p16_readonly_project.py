"""p16: the project's folder cannot be written (a read-only share or checkout): what the sync says, and the run."""
import os, shutil, libsetup
s, team, proj, made, saved = libsetup.setup('p16')
shutil.rmtree(proj + '/Libraries')
open(proj + '/second.sch', 'w').write(open(proj + '/top.sch').read())
os.chmod(proj, 0o555)
try:
    r = s.call('open_document', {'path': proj + '/second.sch'})
    r = s.call('save_document', {'path': 'second.sch'})
    print('save in a read-only project:', s.last_error, str(r)[-400:])
    r = s.call('open_project', {'name': 'use'})
    print('open_project:', s.last_error, str(r)[-300:])
    st = s.call('get_state', {})
    print('Libraries made:', os.path.exists(proj + '/Libraries'))
finally:
    os.chmod(proj, 0o755)
s.close()

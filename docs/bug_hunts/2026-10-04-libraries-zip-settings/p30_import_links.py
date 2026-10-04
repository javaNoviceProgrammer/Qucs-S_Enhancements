"""p30: import_library of a library whose models folder holds a link to a file outside it and a link leading nowhere."""
import os, json, shutil, mcp
s = mcp.Server('p30')
src = s.root + '/vendor'
os.makedirs(src + '/VLib', exist_ok=True)
shutil.copy(os.environ['MYLIB'], src + '/VLib.lib')
open(s.root + '/private.txt', 'w').write('a private file outside the library\n')
os.symlink(s.root + '/private.txt', src + '/VLib/notes.txt')
os.symlink('/nowhere/at/all', src + '/VLib/gone.va')
open(src + '/VLib/opamp_va.va', 'w').write('// va\n')
r = s.call('import_library', {'path': src + '/VLib.lib'})
print('import:', s.last_error, json.dumps(r)[:400].replace(s.root, '<R>'))
dst = s.ws + '/user_lib/VLib'
for f in sorted(os.listdir(dst)) if os.path.isdir(dst) else []:
    p = os.path.join(dst, f); print('  ', f, 'link' if os.path.islink(p) else 'file', open(p).read()[:40].strip() if os.path.isfile(p) else '')
print('user_lib/VLib.lib there:', os.path.isfile(s.ws + '/user_lib/VLib.lib'))
s.close()

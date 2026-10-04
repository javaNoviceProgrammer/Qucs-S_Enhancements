"""p18: the Libraries panel follows its folders: a library added; the folder taken away and made again, a library added
in it; a library renamed - does the panel's list (get_ui dock:Libraries items) show each?"""
import mcp, json, os, shutil, time
def items(s):
    u = s.call('get_ui', {'area': 'dock:Libraries'})
    for c in u.get('controls', []):
        if c.get('items'): return [x for x in c['items'] if 'lib' in x.lower() or x in ('mylib', 'second', 'third', 'renamed')]
    return []
def wait_for(s, name, present=True, secs=4):
    t = time.time()
    while time.time() - t < secs:
        if (name in items(s)) == present: return round(time.time() - t, 2)
        time.sleep(0.25)
    return None
s = mcp.Server('p18')
lib = s.root + '/libs'
os.makedirs(lib, exist_ok=True)
shutil.copy(os.environ['MYLIB'], lib + '/mylib.lib')
s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [lib]}})
print('start:', items(s))
shutil.copy(lib + '/mylib.lib', lib + '/second.lib'); print('second added, shown after', wait_for(s, 'second'))
shutil.rmtree(lib); print('folder gone: mylib hidden after', wait_for(s, 'mylib', False))
os.makedirs(lib); shutil.copy(os.environ['MYLIB'], lib + '/third.lib'); print('folder made again, third shown after', wait_for(s, 'third'))
os.rename(lib + '/third.lib', lib + '/renamed.lib'); print('renamed, shown after', wait_for(s, 'renamed'))
print('end:', items(s))
s.close()

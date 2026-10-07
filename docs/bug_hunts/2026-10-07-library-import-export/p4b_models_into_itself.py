"""p4b: import_library into a search path folder that is the library's own folder of models (NAME/ beside NAME.lib)."""
import os, signal, json, mcp, lib, subprocess
s = mcp.Server('p4b')
team = s.root + '/team'
os.makedirs(team + '/Vendor', exist_ok=True)
open(team + '/Vendor.lib', 'w').write('* vendor\n.subckt VDIV 1 2\nR1 1 2 1k\nR2 2 0 1k\n.ends VDIV\n')
open(team + '/Vendor/readme.txt', 'w').write('models\n')
s.call('set_settings', {'scope': 'app', 'values': {'Locations/Library search paths': [team + '/Vendor']}})
def alarm(*_):
    print('still copying after 60 s; files under', team + '/Vendor:', sum(len(f) for _, _, f in os.walk(team + '/Vendor')))
    s.p.kill()
    raise SystemExit
signal.signal(signal.SIGALRM, alarm)
signal.alarm(60)
r = s.call('import_library', {'path': team + '/Vendor.lib', 'destination': team + '/Vendor'})
signal.alarm(0)
print('answer:', s.last_error, json.dumps(r)[:600].replace(s.root, '<root>'))
depth = max(len(os.path.relpath(r_, team).split(os.sep)) for r_, _, _ in os.walk(team))
print('files under team/Vendor:', sum(len(f) for _, _, f in os.walk(team + '/Vendor')), 'deepest folder level:', depth)
s.close()

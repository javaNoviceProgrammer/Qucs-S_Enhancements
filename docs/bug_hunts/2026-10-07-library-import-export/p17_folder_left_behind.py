"""p17: a Qucs-S library whose parts attach SPICE files, imported without its folder (the colleague sent the .lib alone):
what import_library, describe_part and check_schematic say, and the run."""
import os, re, json, shutil, mcp, lib
from p2_spice_files import FILES, make_sub
s = mcp.Server('p17')
d = lib.project(s, 'fl', FILES)
make_sub(s, d, 'b.sch', 'other/dev.lib', 'DEVB')
r = s.call('create_library', {'name': 'Sent', 'destination': 'project'})
print('made with', r.get('models'))
os.makedirs(s.root + '/mail', exist_ok=True)
shutil.move(d + '/Sent.lib', s.root + '/mail/Sent.lib')      # the .lib alone
shutil.rmtree(d + '/Sent')
r = s.call('import_library', {'path': s.root + '/mail/Sent.lib'})
print('import:', s.last_error, json.dumps({k: r.get(k) for k in ('written', 'note', 'warning')} if isinstance(r, dict) else r).replace(s.root, '<root>')[:400])
dp = s.call('describe_part', {'library': 'Sent', 'part': 'b'})
print('describe_part:', json.dumps(dp)[:300])
def check(s):
    c = s.call('check_schematic', {})
    print('check_schematic:', json.dumps({k: c.get(k) for k in ('errors', 'warnings')} if isinstance(c, dict) else c)[:500])
out = lib.bench(s, r['parts'][0]['place'], d + '/use_b.sch', extra=check)
print('run:', json.dumps(json.loads(out['sim']).get('errors') if 'sim' in out else out)[:400].replace(s.root, '<root>'))
s.close()

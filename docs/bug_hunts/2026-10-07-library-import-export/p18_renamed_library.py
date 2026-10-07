"""p18: a library file renamed before it is imported (Amps.lib -> AmpsTeam.lib, its folder too; as 'also_named' advises, a name
of its own): placed and simulated under ngspice and Qucsator."""
import os, re, json, shutil, mcp, lib
s = mcp.Server('p18')
d = lib.project(s, 'rn', {'div.sch': lib.divider('1k', '3k')})
r = s.call('create_library', {'name': 'Amps', 'destination': 'project'})
os.makedirs(s.root + '/team', exist_ok=True)
shutil.move(d + '/Amps.lib', s.root + '/team/AmpsTeam.lib')
r = s.call('import_library', {'path': s.root + '/team/AmpsTeam.lib'})
print('import:', s.last_error, json.dumps({k: r.get(k) for k in ('library', 'parts')})[:300])
text = open(s.ws + '/user_lib/AmpsTeam.lib').read()
print('in the file:', re.findall(r'\.SUBCKT \S+', text), re.findall(r'\.Def:\S+', text))
for simulator in ['ngspice', 'qucsator']:
    s.call('set_simulator', {'simulator': simulator})
    out = lib.bench(s, r['parts'][0]['place'], d + f'/use_{simulator}.sch')
    v = re.search(r'"value": ([-0-9.e]+)', out.get('v(out)', '')) if 'v(out)' in out else None
    print(f'  {simulator} (want 0.75):', v.group(1) if v else json.dumps(json.loads(out['sim']).get('errors'))[:300])
print('netlist:', [l for l in str(s.call('get_netlist', {})).split('\n') if 'Amps' in l or 'Sub:' in l][:6])
s.close()

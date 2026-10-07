"""p14: a Verilog-A source that cannot be copied into the library (unreadable): create_library analog only, then with digital models."""
import os, re, json, mcp, lib
from p10_verilog_a_elsewhere import VA, CONSTS, SUB   # (runs nothing: guarded below)
s = mcp.Server('p14')
d = lib.project(s, 'em', {'vres.va': VA, 'consts.vh': CONSTS, 'vdiv.sch': SUB})
os.chmod(d + '/consts.vh', 0)
for digital in [False, True]:
    name = f'Em{int(digital)}'
    r = s.call('create_library', {'name': name, 'digital_models': digital})
    msgs = r.get('messages', []) if isinstance(r, dict) else []
    print(f'digital_models {digital}: isError {s.last_error};', [m for m in msgs if re.search(r'(?i)error|cannot|warn|success|embedd', m)],
          '| file there:', os.path.isfile(s.ws + f'/user_lib/{name}.lib'),
          '| folder:', sorted(os.listdir(s.ws + f'/user_lib/{name}')) if os.path.isdir(s.ws + f'/user_lib/{name}') else None)
os.chmod(d + '/consts.vh', 0o644)
s.close()

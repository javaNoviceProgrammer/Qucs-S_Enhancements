"""One .asc through import_netlist, then get_netlist: does the server live? (QUCS picks the build.)"""
import os, shutil, sys, mcp
s = mcp.Server('p10')
shutil.copy(os.path.dirname(os.path.abspath(__file__)) + '/asc_seeds/mysub.asy', s.ws)
f = os.path.abspath(sys.argv[1])
try:
    r = s.call('import_netlist', {'file': f, 'save_as': 'x.sch', 'symbols': s.ws}); print('import:', str(r)[:300])
    r = s.call('get_netlist', {}); print('netlist:', str(r)[:200])
    r = s.call('get_schematic', {}); print('schematic:', str(r)[:200])
    print('alive')
except Exception as e:
    print('DIED:', [l for l in open(s.root + '/server.err', errors='replace').read().splitlines() if 'Fatal' in l or 'runtime error' in l][:3])
s.close()

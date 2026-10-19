import sys, re
sys.path.insert(0, sys.argv[1])
from mcp_client import Client
H = sys.argv[2]
for name in ['shipped.qfem', 'gap_refined.qfem']:
    c = Client(H + '/ind/w')
    p = H + '/ind/' + name
    c.call('open_document', {'path': p})
    err, text = c.call('fem_solve', {'path': p, 'timeout': 300})
    vals = re.findall(r'"(I = [^"]*?: [^"]*?)":"([^"]*)"', text)
    err2, desc = c.call('fem_describe', {'path': p})
    mesh = re.search(r'"mesh":"([^"]*)"', desc)
    print(name, mesh.group(1) if mesh else None, vals[:4], 'ERR ' + text[:150] if err else '', re.findall(r'Warning[^"]*', text)[:2])
    c.close()

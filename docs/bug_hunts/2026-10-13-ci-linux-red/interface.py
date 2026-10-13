import sys, re
# The photodiode's mesh (zzMeshOut's lines): its Ge/Si interface's nodes, each one's medium by
# the first semiconductor triangle touching it (the solver's rule), and by the larger box share.
SEMI = {2: 'slab', 3: 'pplus', 4: 'absorber', 5: 'nplus'}
nodes, tris = [], []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'(MESH[HNTEP]) (.*)', line)
    if not m: continue
    w = m.group(2).split()
    if m.group(1) == 'MESHN': nodes.append((float(w[0]), float(w[1])))
    elif m.group(1) == 'MESHT': tris.append((int(w[0]), int(w[1]), int(w[2]), int(w[3])))
first, share = {}, {}
for a, b, c, d in tris:
    if d not in SEMI: continue
    (xa, ya), (xb, yb), (xc, yc) = nodes[a], nodes[b], nodes[c]
    area = abs((xb - xa) * (yc - ya) - (xc - xa) * (yb - ya)) / 2
    for n in (a, b, c):
        first.setdefault(n, d)
        share.setdefault(n, {}).setdefault(d, 0.0)
        share[n][d] += area / 3
iface = [n for n in share if 2 in share[n] and len(share[n]) > 1]
by_first = {}
by_share = {}
for n in iface:
    by_first[SEMI[first[n]]] = by_first.get(SEMI[first[n]], 0) + 1
    big = max(share[n], key=lambda d: (share[n][d], -d))
    by_share[SEMI[big]] = by_share.get(SEMI[big], 0) + 1
print(f'{len(nodes)} nodes; {len(iface)} on the slab and a germanium domain; by the first triangle {by_first}; by the larger share {by_share}')
for n in sorted(iface, key=lambda n: nodes[n]):
    s = ' '.join(f'{SEMI[d]} {v:.4f}' for d, v in sorted(share[n].items()))
    print(f'  ({nodes[n][0]:.4f}, {nodes[n][1]:.4f}) first {SEMI[first[n]]:8s} share {s}')

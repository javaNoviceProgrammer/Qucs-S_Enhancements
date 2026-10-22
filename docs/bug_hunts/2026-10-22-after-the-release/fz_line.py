"""1D semiconductor read along its line vs a layered 2D reference: random stacks, points between a coarse mesh's nodes."""
import sys, json, random, os
sys.path.insert(0, '/tmp/qucs-hunt/hunt2')
from probe import run, H
seed, count = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
log = open(H + '/fz_line_%d.log' % seed, 'a')
for i in range(count):
    n = rnd.choice([2, 3])
    th = [rnd.choice(['0.05', '0.1', '0.2']) for _ in range(n)]
    mats = [rnd.choice([('Silicon', None), ('GaAs', '0'), ('AlGaAs', '0.3'), ('AlGaAs', '0.15')]) for _ in range(n)]
    if all(m[0] == 'Silicon' for m in mats) or any(m[0] == 'Silicon' for m in mats): mats = [('Silicon', None)] * n
    dop = [(rnd.choice(['donor', 'acceptor']), rnd.choice(['1e15', '1e16', '1e17', '1e18'])) for _ in range(n)]
    Va = rnd.choice(['0', '0.2', '0.5', '-1'])
    geo, y = [], 0.0
    for k in range(n):
        geo.append({"type": "rectangle", "name": "L%d" % k, "position": ["-1", "%g" % y], "size": ["2", th[k]]}); y += float(th[k])
    materials = []
    for k, (m, x) in enumerate(mats):
        d = {"material": "m%d" % k, "selection": {"only": ["L%d" % k]}}
        if m == 'Silicon': d = {"material": "Silicon", "selection": {"only": ["L%d" % k]}}
        else: d.update({"compound": m, "x": x})
        materials.append(d)
    feats = [{"type": "doping", "dopant": dop[k][0], "N0": dop[k][1] + "[1/cm^3]", "selection": {"only": ["L%d" % k]}} for k in range(n)]
    feats += [{"type": "ohmic", "name": "a", "V0": Va + "[V]", "selection": {"of": ["L0"], "side": "bottom"}},
              {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["L%d" % (n - 1)], "side": "top"}}]
    def model(space, extra, mesh):
        ph = {"type": "semiconductor", "tag": "semi", "space": space, "features": feats}; ph.update(extra)
        return {"format": "qucs-s multiphysics 1", "components": [{"unit": "um", "geometry": geo, "materials": materials, "physics": [ph], "mesh": mesh}],
                "studies": [{"type": "study", "tag": "s", "steps": [{"type": "stationary"}]}]}
    ys = sorted(rnd.uniform(0.001, y - 0.001) for _ in range(12))
    def values(m, tag, x):
        calls = [('fem_solve', {'study': 's'})] + [('fem_evaluate', {'study': 's', 'expression': e, 'at': [[x, yy]]}) for yy in ys for e in ('semi.V', 'semi.n', 'semi.p')]
        r = run(m, calls, tag=tag, width=0)
        if r[0][0]: return r[0][1][:150]
        out = {}
        for (err, text), (name, a) in zip(r[1:], calls[1:]):
            try: out[(a['at'][0][1], a['expression'])] = float(json.loads(text)['values'][0]['SI'])
            except Exception: out[(a['at'][0][1], a['expression'])] = None
        return out
    import io, contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        ref = values(model("2d", {}, {"size": "normal", "custom": True, "hmax": "0.5", "features": [{"type": "layered", "hlateral": "0.25", "count": 200, "selection": {"all": True}}]}), 'fzlr', 0)
        one = values(model("1d", {"h1d": "0.5[nm]"}, {"size": "normal", "custom": True, "hmax": "0.08"}), 'fzl1', 0.3)
    if isinstance(ref, str) or isinstance(one, str):
        log.write('refused %d %s %s\n' % (i, ref if isinstance(ref, str) else '', one if isinstance(one, str) else '')); log.flush(); continue
    worst = (0, None)
    for yy in ys:
        for e in ('semi.n', 'semi.p'):
            a, b = one[(yy, e)], ref[(yy, e)]
            if a is None or b is None or b < 1e6: continue
            d = abs(a / b - 1)
            if d > worst[0]: worst = (d, (round(yy, 4), e, a, b))
        a, b = one[(yy, 'semi.V')], ref[(yy, 'semi.V')]
        if a is not None and b is not None and abs(a - b) > 1e-3 and abs(a - b) > worst[0] * 1e-3: worst = max(worst, (abs(a - b) * 100, (round(yy, 4), 'semi.V', a, b)))
    verdict = 'BAD' if worst[0] > 0.05 else 'ok'
    log.write('%s %d mats %s dop %s th %s Va %s worst %.3g at %s\n' % (verdict, i, [m[0] + (m[1] or '') for m in mats], dop, th, Va, worst[0], worst[1])); log.flush()

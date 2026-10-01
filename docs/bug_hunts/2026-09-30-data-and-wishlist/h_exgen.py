# Random datasets for the export fuzz: sweeps, complex, parameter sweeps, op values, empty and mismatched, NaN, odd names.
import random, sys, os, math
out = sys.argv[1]; random.seed(int(sys.argv[2])); count = int(sys.argv[3])
names = ['time', 'frequency', 'R1', 'v(out)', 'tran.v(a,b)', 'S[1,1]', 'x"y', "a'b", 'p;q', 'é', 'n_1', 'ac.i(V1)', 'x.y.z', '_hidden', 'V', 'v']
def num():
    r = random.random()
    if r < 0.03: return 'nan'
    if r < 0.05: return random.choice(['inf', '-inf', '1e308', '-0', '5e-324'])
    return '%r' % random.uniform(-1e3, 1e3)
for k in range(count):
    lines = ['<Qucs Dataset 26.1.4>']
    indeps = {}
    for i in range(random.randint(1, 3)):
        name = random.choice(names) + ('' if random.random() < 0.7 else str(i))
        if name in indeps: continue
        n = random.choice([0, 1, 1, 2, 3, 5, 17])
        indeps[name] = n
        cplx = random.random() < 0.1
        lines.append('<indep %s %d>' % (name, n))
        lines += [num() + ('+j' + num().lstrip('-') if cplx else '') for _ in range(n)]
        lines.append('</indep>')
    for d in range(random.randint(0, 5)):
        deps = random.sample(list(indeps), random.randint(1, len(indeps)))
        size = 1
        for x in deps: size *= indeps[x]
        if random.random() < 0.1: size += random.choice([-1, 1])
        size = max(size, 0)
        cplx = random.random() < 0.4
        name = random.choice(names) + str(d)
        lines.append('<dep %s %s>' % (name, ' '.join(deps)))
        lines += [num() + (('+j' if random.random() < .5 else '-j') + num().lstrip('-') if cplx else '') for _ in range(size)]
        lines.append('</dep>')
    open(os.path.join(out, 'f%04d.dat' % k), 'w').write('\n'.join(lines) + '\n')

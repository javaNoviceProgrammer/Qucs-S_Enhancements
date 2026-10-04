"""p22: check_schematic over every shipped example placing library parts (copied, none run): library-part errors, and
the new warning of two libraries of a name."""
import os, re, shutil, mcp
ex = '<repo>/qucs-s-26.1.1/examples'
bad = re.compile(r'<CMD|\.CUSTOMSIM|NutmegEq|SPICEINIT|shell|system|RunScript=1|Octave')
s = mcp.Server('p22')
n = 0; found = []
for root, dirs, files in sorted(os.walk(ex)):
    for f in sorted(files):
        if not f.endswith('.sch'): continue
        p = os.path.join(root, f); t = open(p, errors='replace').read()
        if '\n  <Lib ' not in t or bad.search(t): continue
        dst = os.path.join(s.ws, os.path.relpath(root, ex)); os.makedirs(dst, exist_ok=True)
        for g in os.listdir(root):
            if os.path.isfile(os.path.join(root, g)) and not os.path.exists(os.path.join(dst, g)): shutil.copy(os.path.join(root, g), dst)
        c = s.call('check_schematic', {'path': os.path.join(dst, f)})
        n += 1
        msgs = [i['message'] for i in (c.get('errors', []) + c.get('warnings', []))] if isinstance(c, dict) else [str(c)[:200]]
        for m in msgs:
            if 'library part' in m or 'more than one library' in m: found.append((os.path.relpath(p, ex), m[:260]))
        s.call('close_document', {'path': os.path.join(dst, f), 'unsaved': 'discard'})
print('examples checked:', n)
for x in found: print(' ', x)
s.close()

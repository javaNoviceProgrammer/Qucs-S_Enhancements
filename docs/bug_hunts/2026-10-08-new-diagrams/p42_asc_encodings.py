"""The LTspice sample in other encodings: UTF-16 LE/BE with and without a BOM, an odd byte count, NULs, Latin-1 µ, CR only - ASan build."""
import os, mcp
here = os.path.dirname(os.path.abspath(__file__))
text = open(here + '/asc_seeds/rc.asc', encoding='utf-8').read()
CASES = {
  'utf-16-le bom': text.encode('utf-16'), 'utf-16-be bom': b'\xfe\xff' + text.encode('utf-16-be'), 'utf-16-le no bom': text.encode('utf-16-le'),
  'utf-16-be no bom': text.encode('utf-16-be'), 'utf-16 odd bytes': text.encode('utf-16')[:-1], 'nul bytes': text.encode('utf-8').replace(b'\n', b'\n\x00', 3),
  'latin-1 mu': text.encode('latin-1', 'replace'), 'cr only': text.replace('\n', '\r').encode(), 'utf-8 bom': b'\xef\xbb\xbf' + text.encode('utf-8'),
  'utf-16 bom only': b'\xff\xfe', 'empty': b'',
}
for name, data in CASES.items():
    s = mcp.Server('p42')
    open(s.ws + '/mysub.asy', 'w').write(open(here + '/asc_seeds/mysub.asy').read())
    open(s.ws + '/t.asc', 'wb').write(data)
    try:
        r = s.call('import_netlist', {'file': s.ws + '/t.asc', 'save_as': 't.sch', 'symbols': s.ws})
        lt = (r.get('LTspice') or [''])[0] if isinstance(r, dict) else str(r)[:140]
        err = open(s.root + '/server.err', errors='replace').read()
        san = [l for l in err.splitlines() if 'runtime error' in l or 'Sanitizer' in l][:2]
        print(f'{name:18s} err={int(s.last_error)} {str(lt)[:110]} {san if san else ""}', flush=True)
    except Exception:
        print(f'{name:18s} SERVER DIED', flush=True)
    s.close()

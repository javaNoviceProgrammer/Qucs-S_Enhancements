#!/usr/bin/env python3
"""fuzz2.py: the deck fuzzer of 23 October (fuzz_pptx.py) with more to it,
run on pptx_probe (the reader and its drawing alone) rather than a test.

    fuzz2.py make <old fuzz_pptx.py> <seeds> <out> <count> <seed>
    fuzz2.py run <old fuzz_pptx.py> <pptx_probe> <out> <findings> [batch] [timeout] [MB]

What it adds to the old mutations: SVG pictures (svgBlip extensions to
missing, wrong, hostile or broken parts; SVGs huge, deep, external, empty),
odd entry names in the archive, duplicate entries, a huge attribute,
namespaces swapped, relationship cycles of layouts and masters, a slide
listed thousands of times, corrupt JPEG, PNG and EMF pictures.
"""
import importlib.util, io, os, random, re, subprocess, sys, threading, time, zipfile

SVGEXT = "{96DAC541-7B7A-43D3-8B79-37D633B846F1}"
ASVG = "http://schemas.microsoft.com/office/drawing/2016/SVG/main"

HOSTILE_SVG = [
    b"",
    b"<svg",
    b"<svg xmlns='http://www.w3.org/2000/svg'/>",
    b"<svg xmlns='http://www.w3.org/2000/svg' width='0' height='0'><rect width='5' height='5'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 0 0'><rect width='5' height='5'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg' viewBox='-1e300 -1e300 1e300 1e300' width='1e9' height='1e9'><rect width='1e308' height='5'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 NaN inf'><circle r='NaN'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><image width='10' height='10' xlink:href='file:///etc/hosts'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><image width='10' height='10' xlink:href='data:image/png;base64,iVBORw0KGgo='/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'>" + b"<g>" * 20000 + b"<rect width='1' height='1'/>" + b"</g>" * 20000 + b"</svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><style>" + b"rect{fill:red}" * 200000 + b"</style><rect width='5' height='5'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><path d='" + b"M0 0 L1 1 " * 300000 + b"'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><filter id='f'><feGaussianBlur stdDeviation='1e9'/></filter><rect width='100' height='100' filter='url(#f)'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><linearGradient id='g' xlink:href='#g' xmlns:xlink='http://www.w3.org/1999/xlink'/><rect width='9' height='9' fill='url(#g)'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><pattern id='p' width='1e-9' height='1e-9' patternUnits='userSpaceOnUse'><rect width='1' height='1'/></pattern><rect width='1000' height='1000' fill='url(#p)'/></svg>",
    b"<svg xmlns='http://www.w3.org/2000/svg'><text x='0' y='10' font-size='1e9'>" + b"W" * 100000 + b"</text></svg>",
    b"\x1f\x8b\x08\x00garbage-gzip",
    b"<?xml version='1.0'?><!DOCTYPE svg SYSTEM 'http://example.invalid/x.dtd'><svg xmlns='http://www.w3.org/2000/svg'><rect width='3' height='3'/></svg>",
]


def load_old(path):
    spec = importlib.util.spec_from_file_location("fuzz_pptx_old", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def blip_ext(rid):
    return '<a:extLst><a:ext uri="%s"><asvg:svgBlip xmlns:asvg="%s" r:embed="%s"/></a:ext></a:extLst>' % (SVGEXT, ASVG, rid)


def mutate_new(entries, rnd):
    """One of the new mutations, on the archive's entries (a dict); its name."""
    names = list(entries)
    slides = [n for n in names if re.match(r"ppt/slides/slide\d+\.xml$", n)]
    kind = rnd.choice(["svgblip", "svgblip", "svgblip", "svgmedia", "svgmedia", "names", "dupentry", "hugeattr", "wrongns",
                       "layoutcycle", "manyslides", "rasterbad", "emfbad", "extjunk", "zerosize"])
    if kind == "svgblip" and slides:
        s = rnd.choice(slides)
        x = entries[s].decode("utf-8", "replace")
        rels = "ppt/slides/_rels/" + s.split("/")[-1] + ".rels"
        target = rnd.choice(["../media/fz.svg", "../media/missing.svg", "slide1.xml", "../slideLayouts/slideLayout1.xml",
                             "../media/" + rnd.choice([n.split("/")[-1] for n in names if "/media/" in n] or ["x.png"]), "http://example.invalid/a.svg", ""])
        if rels in entries:
            r = entries[rels].decode("utf-8", "replace")
            mode = ' TargetMode="External"' if target.startswith("http") else ""
            r = r.replace("</Relationships>", '<Relationship Id="rFz" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="%s"%s/></Relationships>' % (target, mode))
            entries[rels] = r.encode()
        entries["ppt/media/fz.svg"] = rnd.choice(HOSTILE_SVG + [b"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 4 4'><rect width='4' height='2' fill='red'/></svg>"])
        how = rnd.choice(["add", "replace", "pic", "fill", "bg", "nomain"])
        if how in ("add", "replace", "nomain") and "<a:blip " in x:
            if how == "nomain":
                x = re.sub(r'<a:blip [^>]*?(/?)>', lambda m: "<a:blip>" + blip_ext("rFz") + ("</a:blip>" if m.group(1) else ""), x, count=1)
            else:
                x = re.sub(r'(<a:blip [^>]*?)/>', lambda m: m.group(1) + ">" + blip_ext("rFz" if how == "add" else rnd.choice(["rFz", "", "rNone", "rId1"])) + "</a:blip>", x, count=1)
        elif how == "bg" and "<p:cSld>" in x and "<p:bg>" not in x:
            x = x.replace("<p:cSld>", '<p:cSld><p:bg><p:bgPr><a:blipFill><a:blip>' + blip_ext("rFz") + '</a:blip><a:stretch><a:fillRect/></a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg>', 1)
        elif "</p:spTree>" in x:
            pic = ('<p:pic><p:nvPicPr><p:cNvPr id="999" name="fz"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip>%s</a:blip>%s<a:stretch><a:fillRect/></a:stretch></p:blipFill>'
                   '<p:spPr><a:xfrm rot="%s"><a:off x="%s" y="%s"/><a:ext cx="%s" cy="%s"/></a:xfrm><a:prstGeom prst="%s"><a:avLst/></a:prstGeom></p:spPr></p:pic>'
                   % (blip_ext("rFz"), rnd.choice(["", '<a:srcRect l="-50000" t="99999" r="100000" b="-200000"/>', '<a:srcRect l="50000" r="50000"/>']),
                      rnd.choice(["0", "5400000", "-99999999999"]), rnd.choice(["0", "914400", "-914400000"]), rnd.choice(["0", "914400"]),
                      rnd.choice(["0", "1", "914400", "9144000000000", "-5"]), rnd.choice(["0", "1", "914400", "9144000000000"]), rnd.choice(["rect", "ellipse", "cloud"])))
            if how == "fill":
                pic = ('<p:sp><p:nvSpPr><p:cNvPr id="998" name="fzf"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="3000000" cy="2000000"/></a:xfrm>'
                       '<a:prstGeom prst="ellipse"><a:avLst/></a:prstGeom><a:blipFill><a:blip>%s</a:blip>%s</a:blipFill></p:spPr></p:sp>'
                       % (blip_ext("rFz"), rnd.choice(["<a:tile/>", "<a:stretch><a:fillRect/></a:stretch>"])))
            x = x.replace("</p:spTree>", pic + "</p:spTree>", 1)
        entries[s] = x.encode()
        return "svgblip:%s:%s:%s" % (how, s, target)
    if kind == "svgmedia":
        svgs = [n for n in names if n.endswith(".svg")]
        if svgs:
            n = rnd.choice(svgs)
            entries[n] = rnd.choice(HOSTILE_SVG)
            return "svgmedia:" + n
        return "svgmedia:none"
    if kind == "names" and slides:
        s = rnd.choice(slides)
        how = rnd.choice(["backslash", "dotdot", "abs", "upper", "long", "unicode", "dirmark"])
        new = {"backslash": s.replace("/", "\\"), "dotdot": "../../" + s, "abs": "/" + s, "upper": s.upper(), "long": s + "x" * 60000,
               "unicode": s.replace("slide", "sl‮ide"), "dirmark": s + "/"}[how]
        entries[new] = entries.pop(s)
        return "names:%s:%s" % (how, s)
    if kind == "dupentry":
        return "dupentry"   # (done in the archive: see make())
    if kind == "hugeattr" and slides:
        s = rnd.choice(slides)
        x = entries[s].decode("utf-8", "replace")
        x = x.replace("<p:cNvPr ", '<p:cNvPr descr="%s" ' % ("A" * rnd.choice([1000000, 8000000])), 1)
        entries[s] = x.encode()
        return "hugeattr:" + s
    if kind == "wrongns":
        part = rnd.choice(slides + [n for n in names if "slideLayout" in n or "slideMaster" in n or "theme" in n] or names)
        x = entries[part].decode("utf-8", "replace")
        x = x.replace("http://schemas.openxmlformats.org/presentationml/2006/main", "urn:wrong", 1) if rnd.random() < 0.5 else \
            x.replace("http://schemas.openxmlformats.org/drawingml/2006/main", "http://schemas.openxmlformats.org/presentationml/2006/main", 1)
        entries[part] = x.encode()
        return "wrongns:" + part
    if kind == "layoutcycle":
        lr = [n for n in names if re.match(r"ppt/slideLayouts/_rels/.*\.rels$", n)]
        mr = [n for n in names if re.match(r"ppt/slideMasters/_rels/.*\.rels$", n)]
        if lr:
            n = rnd.choice(lr)
            x = entries[n].decode("utf-8", "replace")
            x = re.sub(r'Target="[^"]*slideMaster[^"]*"', 'Target="%s"' % rnd.choice(["../slideLayouts/" + n.split("/")[-1][:-5], "../slides/slide1.xml", "../slideLayouts/slideLayout2.xml"]), x)
            entries[n] = x.encode()
        if mr and rnd.random() < 0.5:
            n = rnd.choice(mr)
            x = entries[n].decode("utf-8", "replace")
            x = re.sub(r'Target="[^"]*theme[^"]*"', 'Target="../slideMasters/slideMaster1.xml"', x)
            entries[n] = x.encode()
        return "layoutcycle"
    if kind == "manyslides" and "ppt/presentation.xml" in entries:
        x = entries["ppt/presentation.xml"].decode("utf-8", "replace")
        m = re.search(r"<p:sldId [^>]*/>", x)
        if m:
            x = x[:m.end()] + m.group(0) * rnd.choice([2000, 6000]) + x[m.end():]
            entries["ppt/presentation.xml"] = x.encode()
        return "manyslides"
    if kind == "rasterbad":
        media = [n for n in names if re.search(r"\.(png|jpe?g|gif|bmp|tiff?)$", n, re.I)]
        if media:
            n = rnd.choice(media)
            d = bytearray(entries[n])
            how = rnd.choice(["header", "trunc", "dims"])
            if how == "header":
                for _ in range(8):
                    if len(d) > 40:
                        d[rnd.randrange(2, 40)] = rnd.randrange(256)
            elif how == "trunc":
                d = d[: rnd.randrange(1, max(2, len(d) // 2))]
            elif d[:4] == b"\x89PNG":
                d[16:24] = (rnd.choice([0, 1, 65535, 0x7fffffff, 0xffffffff])).to_bytes(4, "big") * 2
            entries[n] = bytes(d)
            return "rasterbad:%s:%s" % (how, n)
        return "rasterbad:none"
    if kind == "emfbad" and slides:
        s = rnd.choice(slides)
        rels = "ppt/slides/_rels/" + s.split("/")[-1] + ".rels"
        if rels in entries and "<a:blip " in entries[s].decode("utf-8", "replace"):
            r = entries[rels].decode("utf-8", "replace")
            r = r.replace("</Relationships>", '<Relationship Id="rEmf" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/fz.emf"/></Relationships>')
            entries[rels] = r.encode()
            entries["ppt/media/fz.emf"] = rnd.choice([b"", b"\x01\x00\x00\x00" + b"\xff" * 100, bytes(rnd.randrange(256) for _ in range(5000))])
            x = entries[s].decode("utf-8", "replace")
            entries[s] = re.sub(r'r:embed="[^"]*"', 'r:embed="rEmf"', x, count=1).encode()
        return "emfbad:" + s
    if kind == "extjunk" and slides:
        s = rnd.choice(slides)
        x = entries[s].decode("utf-8", "replace")
        junk = '<a:extLst>' + '<a:ext uri="%s"><asvg:svgBlip xmlns:asvg="%s" r:embed="rId1"/><asvg:svgBlip xmlns:asvg="%s"/></a:ext>' % (SVGEXT, ASVG, ASVG) * rnd.choice([1, 500]) + '</a:extLst>'
        x = re.sub(r"<a:blip([ >])", lambda m: "<a:blip" + m.group(1), x)
        x = x.replace("</a:blip>", junk + "</a:blip>", 1)
        entries[s] = x.encode()
        return "extjunk:" + s
    if kind == "zerosize" and "ppt/presentation.xml" in entries:
        x = entries["ppt/presentation.xml"].decode("utf-8", "replace")
        x = re.sub(r'<p:sldSz [^>]*/>', '<p:sldSz cx="%s" cy="%s"/>' % (rnd.choice(["0", "1", "-9144000", "51206400000", "914400000000"]), rnd.choice(["0", "1", "6858000", "-1"])), x)
        entries["ppt/presentation.xml"] = x.encode()
        return "zerosize"
    return "none:" + kind


def make(old, seeds, out, count, seed):
    rnd = random.Random(seed)
    files = sorted(os.path.join(seeds, f) for f in os.listdir(seeds) if f.endswith(".pptx"))
    os.makedirs(out, exist_ok=True)
    log = []
    for i in range(count):
        src = rnd.choice(files)
        z = zipfile.ZipFile(src)
        entries = {n: z.read(n) for n in z.namelist()}
        notes = []
        dup = False
        if rnd.random() < 0.5:
            for _ in range(rnd.choice([1, 1, 2])):
                k = mutate_new(entries, rnd)
                dup = dup or k == "dupentry"
                notes.append(k)
        else:
            for _ in range(rnd.choice([1, 2])):
                parts = old.xml_parts(list(entries))
                if not parts:
                    break
                name = rnd.choice([p for p in parts if "/slides/" in p or "/charts/" in p] + parts)
                new, kind = old.mutate_text(name, entries[name].decode("utf-8", "replace"), rnd)
                entries[name] = new.encode("utf-8", "surrogatepass")
                notes.append("%s:%s" % (kind, name))
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as w:
            for n, d in entries.items():
                w.writestr(n, d)
            if dup:
                import warnings
                warnings.simplefilter("ignore")
                s = [n for n in entries if "/slides/slide" in n]
                if s:
                    w.writestr(s[0], b"<p:sld xmlns:p='http://schemas.openxmlformats.org/presentationml/2006/main'/>")
        path = os.path.join(out, "n%05d.pptx" % i)
        open(path, "wb").write(buf.getvalue())
        log.append("%s %s %s" % (os.path.basename(path), os.path.basename(src), " ".join(notes)))
    open(os.path.join(out, "mutants.txt"), "w").write("\n".join(log) + "\n")


def rss(pid):
    try:
        return int(subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip() or 0)
    except ValueError:
        return 0


def run(probe, out, logpath, batch=20, timeout=300, limit_mb=6000):
    files = sorted(os.path.join(out, f) for f in os.listdir(out) if f.endswith(".pptx"))
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    findings = open(logpath, "a")
    i = 0
    while i < len(files):
        group = files[i: i + batch]
        start = time.time()
        p = subprocess.Popen([probe] + group, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        lines, peak, killed = [], 0, ""

        def reader():
            for l in p.stdout:
                lines.append(l)
        t = threading.Thread(target=reader, daemon=True)
        t.start()
        last_seen, last_change = 0, time.time()
        while p.poll() is None:
            time.sleep(0.5)
            r = rss(p.pid)
            peak = max(peak, r)
            if len(lines) != last_seen:
                last_seen, last_change = len(lines), time.time()
            if r > limit_mb * 1024:
                killed = "memory %d MB" % (r // 1024)
                p.kill()
            elif time.time() - last_change > timeout:
                killed = "hang %d s" % timeout
                p.kill()
        t.join(5)
        text = "".join(lines)
        started = [l.split("deck: ", 1)[1].strip() for l in lines if l.startswith("deck: ")]
        last = started[-1] if started else group[0]
        bad = killed or ("ERROR: AddressSanitizer" in text and "AddressSanitizer") or ("runtime error" in text and "UBSan") or \
            (p.returncode not in (0, None) and "exit %s" % p.returncode)
        # Slow decks, said apart: one deck's line after its start.
        for k, l in enumerate(lines):
            m = re.search(r"read (\d+) ms, first slide (-?\d+) ms, all (\d+) ms", l)
            if m and int(m.group(1)) + int(m.group(3)) > 20000:
                findings.write("SLOW %s %s" % (lines[k - 1].strip(), l))
        if bad:
            report = [l for l in lines if "ERROR: AddressSanitizer" in l or "runtime error" in l or "SUMMARY" in l][:6]
            findings.write("%s %s (peak %d MB, %.0f s) %s\n" % (os.path.basename(last), bad, peak // 1024, time.time() - start, " | ".join(x.strip()[:300] for x in report)))
            open(logpath + "." + os.path.basename(last) + ".txt", "w").write(text[-30000:])
            i += group.index(last) + 1 if last in group else 1
        else:
            i += len(group)
        findings.flush()
        print("%d/%d %s" % (i, len(files), bad or "ok"), flush=True)
    findings.close()


if __name__ == "__main__":
    old = load_old(sys.argv[2])
    if sys.argv[1] == "make":
        make(old, sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6]))
    else:
        a = sys.argv
        run(a[3], a[4], a[5], int(a[6]) if len(a) > 6 else 20, int(a[7]) if len(a) > 7 else 300, int(a[8]) if len(a) > 8 else 6000)

#!/usr/bin/env python3
"""A deck fuzzer for the PPTX viewer: mutants of seed decks (XML and ZIP
level), each drawn by test_pptx_doc's renderDecksGiven in batches; a crash,
a sanitizer's report, a hang or a memory blow-up pinned to its deck.

    fuzz_pptx.py make <seeds> <out> <count> <seed>
    fuzz_pptx.py run <exe> <out> <log> [batch] [timeout]
"""
import io, os, random, re, subprocess, sys, time, zipfile, zlib

NUMS = ["0", "-1", "-2147483648", "2147483647", "99999999999999", "1e400", "-1e400", "NaN", "inf", "abc", "", "1e-300", "4294967296"]
PRESETS = ["cloud", "cloudCallout", "star32", "arc", "blockArc", "chord", "pie", "donut", "noSmoking", "gear9", "wave", "doubleWave",
           "curvedLeftArrow", "leftUpArrow", "flowChartMagneticTape", "mathNotEqual", "rect", "ellipse", "roundRect", "line",
           "bentConnector5", "curvedConnector4", "wedgeRoundRectCallout", "borderCallout3", "notAShape", "", "triangle", "heart",
           "lightningBolt", "moon", "sun", "teardrop", "frame", "halfFrame", "corner", "diagStripe", "plaque", "can", "cube", "bevel"]
UNICODE = ["‮‭", "é́́́", "\U0001F600" * 50, "​" * 1000, "�", "x" * 200000, "&lt;&gt;&amp;",
           "العربية", "a\u0000b" if False else "a\u0007b", "\t\n" * 500]


def xml_parts(names):
    return [n for n in names if n.endswith(".xml") or n.endswith(".rels")]


def mutate_text(name, x, rnd):
    kind = rnd.choice(["num", "num", "num", "del", "dup", "prst", "text", "nest", "rels", "sldsz", "chart", "table", "custgeom",
                       "shadow", "bullets", "entity", "truncxml", "geom", "geom", "geom"])
    if kind == "num":
        m = list(re.finditer(r'(\s[\w:]+)="(-?\d+(?:\.\d+)?)"', x))
        if m:
            t = rnd.choice(m)
            x = x[:t.start(2)] + rnd.choice(NUMS) + x[t.end(2):]
    elif kind == "del":
        m = list(re.finditer(r"<([\w:]+)[^<>]*/>", x)) + list(re.finditer(r"<([\w:]+)(?:\s[^<>]*)?>[^<]*</\1>", x))
        if m:
            t = rnd.choice(m)
            x = x[:t.start()] + x[t.end():]
    elif kind == "dup":
        m = list(re.finditer(r"<([\w:]+)[^<>]*/>", x))
        if m:
            t = rnd.choice(m)
            x = x[:t.end()] + t.group(0) * rnd.choice([2, 50, 3000]) + x[t.end():]
    elif kind == "prst":
        x = re.sub(r'prst="[^"]*"', lambda _: 'prst="%s"' % rnd.choice(PRESETS), x)
    elif kind == "text":
        m = list(re.finditer(r"<a:t>([^<]*)</a:t>", x))
        if m:
            t = rnd.choice(m)
            x = x[:t.start(1)] + rnd.choice(UNICODE) + x[t.end(1):]
    elif kind == "nest" and "<p:spTree>" in x:
        depth = rnd.choice([5, 20, 40])
        chext = rnd.choice(['cx="0" cy="0"', 'cx="100" cy="100"', 'cx="-5" cy="1"'])
        g = ('<p:grpSp><p:nvGrpSpPr><p:cNvPr id="9" name="g"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/>'
             '<a:ext cx="914400" cy="914400"/><a:chOff x="0" y="0"/><a:chExt %s/></a:xfrm></p:grpSpPr>' % chext)
        inner = ('<p:sp><p:nvSpPr><p:cNvPr id="10" name="s"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="50" cy="50"/>'
                 '</a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:sp>')
        x = x.replace("<p:spTree>", "<p:spTree>" + g * depth + inner + "</p:grpSp>" * depth, 1)
    elif kind == "rels" and name.endswith(".rels"):
        m = list(re.finditer(r'Target="([^"]*)"', x))
        if m:
            t = rnd.choice(m)
            target = rnd.choice(["", "../../../../../../etc/passwd", "slide1.xml", "../slides/slide1.xml", "../slideLayouts/slideLayout1.xml",
                                 "missing.xml", "http://example.invalid/x.png", "../media/image1.png", "/ppt/slides/slide1.xml", "../charts/chart1.xml"])
            x = x[:t.start(1)] + target + x[t.end(1):]
    elif kind == "sldsz" and "sldSz" in x:
        x = re.sub(r'<p:sldSz cx="\d+" cy="\d+"', '<p:sldSz cx="%s" cy="%s"' % (rnd.choice(NUMS), rnd.choice(NUMS)), x)
    elif kind == "chart" and "c:pt " in x:
        x = re.sub(r'<c:pt idx="\d+"', lambda _: '<c:pt idx="%s"' % rnd.choice(["2000000000", "-1", "0", "99999999"]), x, count=rnd.choice([1, 3]))
        x = re.sub(r'<c:ptCount val="\d+"', lambda _: '<c:ptCount val="%s"' % rnd.choice(["2000000000", "0", "-5"]), x, count=1)
        x = re.sub(r"<c:v>[^<]*</c:v>", lambda _: "<c:v>%s</c:v>" % rnd.choice(["NaN", "inf", "-inf", "1e308", "x", "-1e308"]), x, count=rnd.choice([1, 5]))
    elif kind == "table" and "<a:tbl>" in x:
        x = re.sub(r'<a:gridCol w="\d+"', lambda _: '<a:gridCol w="%s"' % rnd.choice(["0", "-100", "999999999999"]), x, count=2)
        x = re.sub(r"<a:tc>", lambda _: '<a:tc gridSpan="%s" rowSpan="%s">' % (rnd.choice(["0", "1000000", "-3"]), rnd.choice(["0", "1000000", "-3"])), x, count=2)
    elif kind == "custgeom" and "<a:prstGeom" in x:
        geo = ('<a:custGeom><a:pathLst><a:path w="%s" h="%s"><a:moveTo><a:pt x="0" y="0"/></a:moveTo>'
               '<a:arcTo wR="%s" hR="%s" stAng="%s" swAng="%s"/><a:lnTo><a:pt x="%s" y="5"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>'
               % (rnd.choice(["0", "100", "-1"]), rnd.choice(["0", "100"]), rnd.choice(["0", "-50", "50", "1e300"]), rnd.choice(["0", "50"]),
                  rnd.choice(["0", "21600000", "-99999999999"]), rnd.choice(["2160000000000", "0", "-21600000", "NaN"]), rnd.choice(["1e308", "5"])))
        x = re.sub(r"<a:prstGeom[^>]*>.*?</a:prstGeom>", geo, x, count=1, flags=re.S)
    elif kind == "shadow" and "</p:spPr>" in x:
        sh = ('<a:effectLst><a:outerShdw blurRad="%s" dist="%s" dir="%s" sx="%s" sy="%s" algn="tl"><a:srgbClr val="000000"/></a:outerShdw></a:effectLst>'
              % (rnd.choice(["12700000000", "0", "-5", "50800000"]), rnd.choice(["12700000000", "0", "-12700000"]), rnd.choice(["5400000", "-1", "99999999999"]),
                 rnd.choice(["100000", "0", "-100000", "99999999"]), rnd.choice(["100000", "0", "99999999"])))
        x = x.replace("</p:spPr>", sh + "</p:spPr>", 1)
    elif kind == "bullets" and "<a:p>" in x:
        b = '<a:pPr lvl="%s" marL="%s" indent="%s"><a:buAutoNum type="%s" startAt="%s"/><a:lnSpc><a:spcPct val="%s"/></a:lnSpc></a:pPr>' % (
            rnd.choice(["0", "8", "1000", "-1"]), rnd.choice(["0", "-99999999999", "99999999999"]), rnd.choice(["0", "-99999999999"]),
            rnd.choice(["romanUcPeriod", "alphaLcParenR", "arabicPeriod", "circleNumDbPlain", "bogus"]), rnd.choice(["1", "2147483647", "-5", "0"]),
            rnd.choice(["100000", "0", "99999999", "-100000"]))
        x = x.replace("<a:p>", "<a:p>" + b, rnd.choice([1, 5]))
    elif kind == "entity":
        dtd = '<!DOCTYPE x [<!ENTITY a "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;"><!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;"><!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;"><!ENTITY e "&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;"><!ENTITY f "&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;">]>'
        x = re.sub(r"^(<\?xml[^>]*\?>)?", lambda m: (m.group(1) or "") + dtd, x, count=1)
        x = x.replace("<a:t>", "<a:t>&f;", 1)
    elif kind == "geom":
        big = ["99999999999999", "-99999999999999", "2147483647", "0", "1", "51206400", "914400000000", "-914400000000"]
        m = list(re.finditer(r'<a:(off|ext|chOff|chExt) (x|cx)="(-?\d+)" (y|cy)="(-?\d+)"', x))
        for t in rnd.sample(m, min(len(m), rnd.choice([1, 2, 5]))):
            x = x[:t.start(3)] + rnd.choice(big) + x[t.end(3):t.start(5)] + rnd.choice(big) + x[t.end(5):]
            break
        if rnd.random() < 0.5:
            x = re.sub(r"<a:ln( [^>]*)?>", lambda _: '<a:ln w="%s"><a:prstDash val="%s"/>' % (rnd.choice(["1", "0", "12700000000", "99999999999", "-5"]), rnd.choice(["sysDot", "dash", "lgDashDotDot", "bogus"])), x, count=rnd.choice([1, 3]))
            x = x.replace('<a:prstDash val="sysDot"/><a:prstDash', '<a:prstDash')
        if rnd.random() < 0.4:
            x = re.sub(r'<a:xfrm( [^>]*)?>', lambda _: '<a:xfrm rot="%s" flipH="1">' % rnd.choice(["99999999999", "-21600000", "5400000", "1"]), x, count=rnd.choice([1, 3]))
    elif kind == "truncxml":
        x = x[: rnd.randrange(max(1, len(x)))]
    return x, kind


def mutate_media(data, rnd):
    kind = rnd.choice(["trunc", "flip", "svgbomb", "huge_png"])
    if kind == "trunc":
        return data[: rnd.randrange(max(1, len(data)))], kind
    if kind == "flip":
        b = bytearray(data)
        for _ in range(rnd.choice([1, 10, 100])):
            if b:
                b[rnd.randrange(len(b))] = rnd.randrange(256)
        return bytes(b), kind
    if kind == "svgbomb":
        return (b'<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;"><!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;">'
                b'<!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;"><!ENTITY e "&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;"><!ENTITY f "&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;">]>'
                b'<svg xmlns="http://www.w3.org/2000/svg" width="100000000" height="100000000"><text>&f;&f;&f;</text></svg>'), kind
    # A PNG that says it is 100000 x 100000.
    ihdr = b"IHDR" + (100000).to_bytes(4, "big") * 2 + bytes([8, 6, 0, 0, 0])
    png = b"\x89PNG\r\n\x1a\n" + len(ihdr[4:]).to_bytes(4, "big") + ihdr + zlib.crc32(ihdr).to_bytes(4, "big")
    idat = zlib.compress(b"\0" * 1000)
    png += len(idat).to_bytes(4, "big") + b"IDAT" + idat + zlib.crc32(b"IDAT" + idat).to_bytes(4, "big") + b"\0\0\0\0IEND\xaeB`\x82"
    return png, kind


def make(seeds, out, count, seed):
    rnd = random.Random(seed)
    files = sorted(os.path.join(seeds, f) for f in os.listdir(seeds) if f.endswith(".pptx"))
    os.makedirs(out, exist_ok=True)
    log = []
    for i in range(count):
        src = rnd.choice(files)
        z = zipfile.ZipFile(src)
        entries = {n: z.read(n) for n in z.namelist()}
        notes = []
        level = rnd.random()
        if level < 0.82:
            for _ in range(rnd.choice([1, 1, 2, 3])):
                parts = xml_parts(list(entries))
                name = rnd.choice([p for p in parts if "/slides/" in p or "/charts/" in p] + parts)
                text = entries[name].decode("utf-8", "replace")
                new, kind = mutate_text(name, text, rnd)
                entries[name] = new.encode("utf-8", "surrogatepass")
                notes.append("%s:%s" % (kind, name))
        elif level < 0.92:
            media = [n for n in entries if "/media/" in n]
            if media:
                name = rnd.choice(media)
                entries[name], kind = mutate_media(entries[name], rnd)
                if kind == "svgbomb":
                    # (Named .svg: sniffed by its bytes anyway.)
                    pass
                notes.append("%s:%s" % (kind, name))
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as w:
            for n, d in entries.items():
                w.writestr(n, d)
            if level >= 0.92 and level < 0.96:
                # A deflate bomb beside the parts: 600 MB of zeros.
                w.writestr("ppt/media/bomb.bin", b"\0" * (600 * 1024 * 1024))
                notes.append("bomb")
        data = buf.getvalue()
        if level >= 0.96:
            b = bytearray(data)
            how = rnd.choice(["truncate", "flip_tail"])
            if how == "truncate":
                b = b[: rnd.randrange(max(1, len(b)))]
            else:
                for _ in range(20):
                    k = len(b) - 1 - rnd.randrange(min(len(b), 2000))
                    b[k] = rnd.randrange(256)
            data = bytes(b)
            notes.append("zip:" + how)
        path = os.path.join(out, "m%05d.pptx" % i)
        open(path, "wb").write(data)
        log.append("%s %s %s" % (os.path.basename(path), os.path.basename(src), " ".join(notes)))
    open(os.path.join(out, "mutants.txt"), "w").write("\n".join(log) + "\n")


def rss(pid):
    try:
        return int(subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip() or 0)
    except ValueError:
        return 0


def run(exe, out, logpath, batch=20, timeout=300):
    files = sorted(os.path.join(out, f) for f in os.listdir(out) if f.endswith(".pptx"))
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_PPTX_OUT="-", ASAN_OPTIONS="detect_leaks=0")
    env["QUCS_CACHE_DIR"] = os.path.join(os.path.dirname(out), "cache")
    findings = open(logpath, "a")
    i = 0
    while i < len(files):
        group = files[i : i + batch]
        env["QUCS_PPTX_RENDER"] = ";".join(group)
        start = time.time()
        p = subprocess.Popen([exe, "renderDecksGiven", "-maxwarnings", "0"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        lines = []
        peak = 0
        killed = ""
        import threading
        def reader():
            for l in p.stdout:
                lines.append(l)
        t = threading.Thread(target=reader, daemon=True)
        t.start()
        while p.poll() is None:
            time.sleep(1)
            r = rss(p.pid)
            peak = max(peak, r)
            if r > 6_000_000:
                killed = "memory %d MB" % (r // 1024)
                p.kill()
            elif time.time() - start > timeout:
                killed = "timeout %d s" % timeout
                p.kill()
        t.join(5)
        text = "".join(lines)
        started = [l.split("deck: ", 1)[1].strip() for l in lines if "deck: " in l]
        last = started[-1] if started else group[0]
        bad = killed or ("ERROR: AddressSanitizer" in text and "AddressSanitizer") or ("runtime error" in text and "UBSan") or (p.returncode not in (0, None) and "exit %s" % p.returncode)
        if bad:
            report = [l for l in lines if "ERROR: AddressSanitizer" in l or "runtime error" in l or "SUMMARY" in l][:6]
            findings.write("%s %s (peak %d MB, %.0f s) %s\n" % (os.path.basename(last), bad, peak // 1024, time.time() - start, " | ".join(x.strip()[:300] for x in report)))
            findings.flush()
            open(logpath + "." + os.path.basename(last) + ".txt", "w").write(text[-20000:])
            # Go on after the deck that failed.
            done = group.index(last) + 1 if last in group else 1
            i += done
        else:
            i += len(group)
        print("%d/%d %s" % (i, len(files), bad or "ok"), flush=True)
    findings.close()


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
    else:
        run(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 20, int(sys.argv[6]) if len(sys.argv) > 6 else 300)

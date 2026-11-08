#!/usr/bin/env python3
"""mkdecks.py <out>: decks for timing the viewer and for its SVG pictures.

  text10.pptx       10 slides of titles and bullets
  photos200.pptx    200 slides, each a title, bullets and its own 2400 x 1600
                    JPEG (about 1 MB each; made with sips)
  shapes50.pptx     50 slides of 300 shapes each, with text
  svg_office.pptx   an SVG as PowerPoint 365 writes it: a PNG blip, the SVG
                    in the blip's extension list
  svg_mac.pptx      an SVG as PowerPoint for Mac writes an icon: the SVG in
                    the extension list alone, no main blip
  svg_main.pptx     an SVG as the main blip (LibreOffice, other tools)
  svg_bad.pptx      a broken SVG with a PNG fallback; a broken SVG alone; an
                    SVG asking too much through <use>, with its fallback
  svg_fill.pptx     an SVG as a shape's picture fill, and as the background
"""
import io, os, random, struct, subprocess, sys, zipfile, zlib

NS = ('xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
      'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"')
REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
SVGEXT = "{96DAC541-7B7A-43D3-8B79-37D633B846F1}"


def emu(pt):
    return str(int(round(pt * 12700)))


def xfrm(x, y, w, h):
    return '<a:xfrm><a:off x="%s" y="%s"/><a:ext cx="%s" cy="%s"/></a:xfrm>' % (emu(x), emu(y), emu(w), emu(h))


def sp(i, x, y, w, h, txt="", fill="4472C4", ph=""):
    body = ""
    if txt:
        body = '<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang="en-US" sz="1400"/><a:t>%s</a:t></a:r></a:p></p:txBody>' % txt
    geom = "" if ph else xfrm(x, y, w, h) + '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val="%s"/></a:solidFill>' % fill
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="s%d"/><p:cNvSpPr/><p:nvPr>%s</p:nvPr></p:nvSpPr><p:spPr>%s</p:spPr>%s</p:sp>'
            % (i, i, ph, geom, body))


def bullets(i, lines):
    ps = "".join('<a:p><a:r><a:rPr lang="en-US"/><a:t>%s</a:t></a:r></a:p>' % l for l in lines)
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="Body"/><p:cNvSpPr/><p:nvPr><p:ph idx="1"/></p:nvPr></p:nvSpPr><p:spPr/>'
            '<p:txBody><a:bodyPr/><a:lstStyle/>%s</p:txBody></p:sp>' % (i, ps))


def title(i, t):
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="Title"/><p:cNvSpPr/><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr><p:spPr/>'
            '<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang="en-US"/><a:t>%s</a:t></a:r></a:p></p:txBody></p:sp>' % (i, t))


def pic(i, x, y, w, h, embed=None, svg=None, name="Picture"):
    ext = ""
    if svg:
        ext = ('<a:extLst><a:ext uri="%s"><asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed="%s"/>'
               '</a:ext></a:extLst>' % (SVGEXT, svg))
    blip = '<a:blip%s>%s</a:blip>' % (' r:embed="%s"' % embed if embed else "", ext)
    return ('<p:pic><p:nvPicPr><p:cNvPr id="%d" name="%s %d"/><p:cNvPicPr><a:picLocks noChangeAspect="1"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>'
            '<p:blipFill>%s<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>%s<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:pic>'
            % (i, name, i, blip, xfrm(x, y, w, h)))


def deck(path, slides, media, size=(960, 540)):
    """slides: [(tree, rels: [(id, target)], bg)]; media: {name: bytes}"""
    z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
    w = lambda n, s: z.writestr(n, s if isinstance(s, bytes) else s.encode())
    w("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
      '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
      '<Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/>'
      '<Default Extension="jpeg" ContentType="image/jpeg"/><Default Extension="svg" ContentType="image/svg+xml"/>'
      '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/></Types>')
    w("_rels/.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
      '<Relationship Id="rId1" Type="%s/officeDocument" Target="ppt/presentation.xml"/></Relationships>' % REL)
    ids = "".join('<p:sldId id="%d" r:id="rS%d"/>' % (256 + i, i + 1) for i in range(len(slides)))
    rels = "".join('<Relationship Id="rS%d" Type="%s/slide" Target="slides/slide%d.xml"/>' % (i + 1, REL, i + 1) for i in range(len(slides)))
    w("ppt/presentation.xml", '<?xml version="1.0"?><p:presentation %s><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rM"/></p:sldMasterIdLst>'
      '<p:sldIdLst>%s</p:sldIdLst><p:sldSz cx="%s" cy="%s"/><p:notesSz cx="6858000" cy="9144000"/></p:presentation>' % (NS, ids, emu(size[0]), emu(size[1])))
    w("ppt/_rels/presentation.xml.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">%s'
      '<Relationship Id="rM" Type="%s/slideMaster" Target="slideMasters/slideMaster1.xml"/></Relationships>' % (rels, REL))
    w("ppt/theme/theme1.xml", '<?xml version="1.0"?><a:theme %s name="T"><a:themeElements><a:clrScheme name="C">'
      '<a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="1F2A44"/></a:dk2><a:lt2><a:srgbClr val="EEECE1"/></a:lt2>'
      '<a:accent1><a:srgbClr val="4472C4"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2><a:accent3><a:srgbClr val="A5A5A5"/></a:accent3>'
      '<a:accent4><a:srgbClr val="FFC000"/></a:accent4><a:accent5><a:srgbClr val="5B9BD5"/></a:accent5><a:accent6><a:srgbClr val="70AD47"/></a:accent6>'
      '<a:hlink><a:srgbClr val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>'
      '<a:fontScheme name="F"><a:majorFont><a:latin typeface="Helvetica"/></a:majorFont><a:minorFont><a:latin typeface="Helvetica"/></a:minorFont></a:fontScheme>'
      '<a:fmtScheme name="S"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w="12700"/></a:lnStyleLst>'
      '<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
      '</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>' % NS)
    tree0 = '<p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>'
    w("ppt/slideMasters/slideMaster1.xml", '<?xml version="1.0"?><p:sldMaster %s><p:cSld><p:bg><p:bgRef idx="1001"><a:schemeClr val="bg1"/></p:bgRef></p:bg>'
      '<p:spTree>%s<p:sp><p:nvSpPr><p:cNvPr id="2" name="T"/><p:cNvSpPr/><p:nvPr><p:ph type="title"/></p:nvPr></p:nvSpPr><p:spPr>%s</p:spPr></p:sp>'
      '<p:sp><p:nvSpPr><p:cNvPr id="3" name="B"/><p:cNvSpPr/><p:nvPr><p:ph type="body" idx="1"/></p:nvPr></p:nvSpPr><p:spPr>%s</p:spPr></p:sp></p:spTree></p:cSld>'
      '<p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" '
      'accent6="accent6" hlink="hlink" folHlink="folHlink"/><p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rL"/></p:sldLayoutIdLst>'
      '<p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz="4000"><a:solidFill><a:schemeClr val="tx2"/></a:solidFill><a:latin typeface="+mj-lt"/></a:defRPr></a:lvl1pPr></p:titleStyle>'
      '<p:bodyStyle><a:lvl1pPr marL="228600" indent="-228600"><a:buChar char="&#8226;"/><a:defRPr sz="2400"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr></p:bodyStyle>'
      '<p:otherStyle><a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>' % (NS, tree0, xfrm(48, 30, 864, 80), xfrm(48, 130, 864, 380)))
    w("ppt/slideMasters/_rels/slideMaster1.xml.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
      '<Relationship Id="rL" Type="%s/slideLayout" Target="../slideLayouts/slideLayout1.xml"/><Relationship Id="rT" Type="%s/theme" Target="../theme/theme1.xml"/></Relationships>' % (REL, REL))
    w("ppt/slideLayouts/slideLayout1.xml", '<?xml version="1.0"?><p:sldLayout %s><p:cSld><p:spTree>%s</p:spTree></p:cSld></p:sldLayout>' % (NS, tree0))
    w("ppt/slideLayouts/_rels/slideLayout1.xml.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
      '<Relationship Id="rM" Type="%s/slideMaster" Target="../slideMasters/slideMaster1.xml"/></Relationships>' % REL)
    for i, (tree, srels, bg) in enumerate(slides):
        w("ppt/slides/slide%d.xml" % (i + 1), '<?xml version="1.0"?><p:sld %s><p:cSld>%s<p:spTree>%s%s</p:spTree></p:cSld></p:sld>' % (NS, bg or "", tree0, tree))
        r = '<Relationship Id="rL" Type="%s/slideLayout" Target="../slideLayouts/slideLayout1.xml"/>' % REL
        r += "".join('<Relationship Id="%s" Type="%s/image" Target="../media/%s"/>' % (rid, REL, t) for rid, t in srels)
        w("ppt/slides/_rels/slide%d.xml.rels" % (i + 1), '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">%s</Relationships>' % r)
    for n, b in media.items():
        z.writestr("ppt/media/" + n, b, compress_type=zipfile.ZIP_STORED if n.endswith((".jpeg", ".png")) else zipfile.ZIP_DEFLATED)
    z.close()


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def jpegs(out, n, w=2400, h=1600):
    """n distinct photo-like JPEGs: a noisy 300 x 200 picture, scaled up by sips."""
    tmp = os.path.join(out, "_jpg")
    os.makedirs(tmp, exist_ok=True)
    res = []
    rnd = random.Random(5)
    for k in range(n):
        ppm = os.path.join(tmp, "p%d.ppm" % k)
        jpg = os.path.join(tmp, "p%d.jpeg" % k)
        if not os.path.exists(jpg):
            sw, sh = 300, 200
            px = bytearray()
            for y in range(sh):
                for x in range(sw):
                    px += bytes(((x * 255 // sw + k * 37 + rnd.randrange(60)) & 255, (y * 255 // sh + rnd.randrange(60)) & 255, ((x + y + k * 50) & 255)))
            with open(ppm, "wb") as f:
                f.write(b"P6 %d %d 255\n" % (sw, sh) + bytes(px))
            subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", "--resampleHeightWidth", str(h), str(w), ppm, "--out", jpg],
                           capture_output=True, check=True)
            os.remove(ppm)
        res.append(open(jpg, "rb").read())
    return res


SVG_ICON = ('<svg width="96" height="96" viewBox="0 0 96 96" xmlns="http://www.w3.org/2000/svg" overflow="hidden">'
            '<rect x="0" y="0" width="48" height="96" fill="#FF0000"/><rect x="48" y="0" width="48" height="96" fill="#0000FF"/>'
            '<path d="M48 10 L86 86 L10 86 Z" fill="#00FF00"/></svg>')
SVG_THIN = ('<svg viewBox="0 0 1000 1000" xmlns="http://www.w3.org/2000/svg"><rect width="1000" height="1000" fill="white"/>'
            + "".join('<line x1="0" y1="%d" x2="1000" y2="%d" stroke="black" stroke-width="1"/>' % (y, y) for y in range(5, 1000, 10)) + '</svg>')


def main(out):
    os.makedirs(out, exist_ok=True)
    # text10
    sl = [(title(2, "Slide %d of ten" % (i + 1)) + bullets(3, ["Point %d.%d about the circuit and its response" % (i + 1, k) for k in range(6)]), [], None) for i in range(10)]
    deck(os.path.join(out, "text10.pptx"), sl, {})
    # text3000
    sl = [(title(2, "Slide %d" % (i + 1)) + bullets(3, ["Point %d.%d" % (i + 1, k) for k in range(4)]), [], None) for i in range(3000)]
    deck(os.path.join(out, "text3000.pptx"), sl, {})
    # shapes50
    sl = []
    for i in range(50):
        t = "".join(sp(10 + k, (k % 20) * 46 + 10, (k // 20) * 34 + 20, 40, 28, "s%d" % k, "%06X" % ((k * 2654435761) & 0xFFFFFF)) for k in range(300))
        sl.append((t, [], None))
    deck(os.path.join(out, "shapes50.pptx"), sl, {})
    # photos200
    photos = jpegs(out, 20)
    sl, media = [], {}
    for i in range(200):
        name = "image%d.jpeg" % (i + 1)
        media[name] = photos[i % 20]
        sl.append((title(2, "Photo %d" % (i + 1)) + bullets(3, ["Taken at site %d" % i, "Exposure and notes"]) + pic(4, 480, 130, 450, 300, embed="rP"), [("rP", name)], None))
    deck(os.path.join(out, "photos200.pptx"), sl, media)
    # SVG decks
    fallback = png(96, 96, (128, 128, 128))
    deck(os.path.join(out, "svg_office.pptx"), [(pic(2, 100, 100, 300, 300, embed="rPng", svg="rSvg"), [("rPng", "image1.png"), ("rSvg", "image2.svg")], None)],
         {"image1.png": fallback, "image2.svg": SVG_ICON.encode()})
    deck(os.path.join(out, "svg_mac.pptx"), [(pic(2, 100, 100, 300, 300, svg="rSvg", name="Graphic"), [("rSvg", "image1.svg")], None)], {"image1.svg": SVG_ICON.encode()})
    deck(os.path.join(out, "svg_main.pptx"), [(pic(2, 100, 100, 300, 300, embed="rSvg"), [("rSvg", "image1.svg")], None)], {"image1.svg": SVG_ICON.encode()})
    expand = '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"><defs><rect id="l0" width="1" height="1"/>' + "".join(
        '<g id="l%d"><use xlink:href="#l%d"/><use xlink:href="#l%d"/><use xlink:href="#l%d"/><use xlink:href="#l%d"/></g>' % (k, k - 1, k - 1, k - 1, k - 1) for k in range(1, 16)) + '</defs><use xlink:href="#l15"/></svg>'
    deck(os.path.join(out, "svg_bad.pptx"),
         [(pic(2, 50, 50, 200, 200, embed="rPng", svg="rBad") + pic(3, 300, 50, 200, 200, svg="rBad2") + pic(4, 550, 50, 200, 200, embed="rPng", svg="rExp")
           + pic(5, 50, 300, 200, 200, embed="rPng", svg="rMissing"),
           [("rPng", "image1.png"), ("rBad", "image2.svg"), ("rBad2", "image3.svg"), ("rExp", "image4.svg"), ("rMissing", "image9.svg")], None)],
         {"image1.png": fallback, "image2.svg": b"<svg xmlns='http://www.w3.org/2000/svg'><rect width='10'", "image3.svg": b"\x00\x01not svg", "image4.svg": expand.encode()})
    fillsp = ('<p:sp><p:nvSpPr><p:cNvPr id="2" name="F"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>%s<a:prstGeom prst="ellipse"><a:avLst/></a:prstGeom>'
              '<a:blipFill><a:blip r:embed="rPng"><a:extLst><a:ext uri="%s"><asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed="rSvg"/></a:ext></a:extLst></a:blip>'
              '<a:stretch><a:fillRect/></a:stretch></a:blipFill></p:spPr></p:sp>' % (xfrm(500, 100, 300, 300), SVGEXT))
    bg = ('<p:bg><p:bgPr><a:blipFill><a:blip><a:extLst><a:ext uri="%s"><asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed="rThin"/></a:ext></a:extLst></a:blip>'
          '<a:stretch><a:fillRect/></a:stretch></a:blipFill><a:effectLst/></p:bgPr></p:bg>' % SVGEXT)
    deck(os.path.join(out, "svg_fill.pptx"), [(fillsp, [("rPng", "image1.png"), ("rSvg", "image2.svg"), ("rThin", "image3.svg")], bg)],
         {"image1.png": fallback, "image2.svg": SVG_ICON.encode(), "image3.svg": SVG_THIN.encode()})


if __name__ == "__main__":
    main(sys.argv[1])

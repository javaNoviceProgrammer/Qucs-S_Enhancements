import sys, zipfile, re
src, outdir = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(src); base = {n: z.read(n) for n in z.namelist()}
def emit(name, changes, extra=None):
    e = dict(base)
    for part, (old, new) in changes.items():
        x = e[part].decode()
        assert old in x, (name, part, old)
        e[part] = x.replace(old, new).encode()
    if extra: e.update(extra)
    with zipfile.ZipFile("%s/%s.pptx" % (outdir, name), "w", zipfile.ZIP_DEFLATED) as w:
        for n, d in e.items(): w.writestr(n, d)
L = "ppt/slideLayouts/_rels/slideLayout1.xml.rels"; S = "ppt/slides/_rels/slide1.xml.rels"; M = "ppt/slideMasters/_rels/slideMaster1.xml.rels"
emit("cyc_layout_self", {L: ("../slideMasters/slideMaster1.xml", "../slideLayouts/slideLayout1.xml")})
emit("cyc_slide_self", {S: ("../slideLayouts/slideLayout1.xml", "../slides/slide1.xml")})
emit("cyc_layout_slide", {L: ("../slideMasters/slideMaster1.xml", "../slides/slide1.xml")})
m = base[M].decode()
t = re.search(r'Type="[^"]*/theme" Target="([^"]*)"', m)
if t: emit("cyc_master_theme_is_master", {M: (t.group(0), t.group(0).replace(t.group(1), "slideMaster1.xml"))})
emit("cyc_notes_self", {S: ("../notesSlides/notesSlide1.xml", "../slides/slide1.xml")})
C = "ppt/charts/_rels/chart1.xml.rels"
emit("chart_is_slide", {"ppt/slides/_rels/slide4.xml.rels": ("../charts/chart1.xml", "../slides/slide4.xml")})
emit("layout_is_chart", {S: ("../slideLayouts/slideLayout1.xml", "../charts/chart1.xml")})
emit("layout_is_bin", {S: ("../slideLayouts/slideLayout1.xml", "../printerSettings/printerSettings1.bin")})
emit("layout_is_xlsx", {S: ("../slideLayouts/slideLayout1.xml", "../embeddings/Microsoft_Excel_Sheet1.xlsx")})
P = "ppt/_rels/presentation.xml.rels"
p = base[P].decode()
# Every slide the same part, 2000 times: sldIdLst grown.
pres = base["ppt/presentation.xml"].decode()
ids = re.search(r"<p:sldIdLst>(.*?)</p:sldIdLst>", pres).group(1)
first = re.search(r'<p:sldId [^>]*/>', ids).group(0)
many = "".join(first for _ in range(3000))
emit("same_slide_3000", {"ppt/presentation.xml": (ids, many)})

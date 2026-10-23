import sys, zipfile, re
src, out, part, how = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
z = zipfile.ZipFile(src); e = {n: z.read(n) for n in z.namelist()}
x = e[part].decode()
if how.startswith("deep"):
    d = int(how[4:])
    x = x.replace("<p:spTree>", "<p:spTree>" + "<a:x>" * d + "</a:x>" * d, 1)
elif how.startswith("groups"):
    d = int(how[6:])
    g = '<p:grpSp><p:nvGrpSpPr><p:cNvPr id="9" name="g"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="914400" cy="914400"/><a:chOff x="0" y="0"/><a:chExt cx="914400" cy="914400"/></a:xfrm></p:grpSpPr>'
    x = x.replace("<p:spTree>", "<p:spTree>" + g * d + "</p:grpSp>" * d, 1)
elif how.startswith("ptcount"):
    x = re.sub(r'<c:ptCount val="\d+"', '<c:ptCount val="%s"' % how[7:], x, count=1)
elif how.startswith("alt"):
    d = int(how[3:])
    x = x.replace("<p:spTree>", '<p:spTree>' + '<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Fallback>' * d + '</mc:Fallback></mc:AlternateContent>' * d, 1)
e[part] = x.encode()
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as w:
    for n, d in e.items(): w.writestr(n, d)

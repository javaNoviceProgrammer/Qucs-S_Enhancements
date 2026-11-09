#!/usr/bin/env python3
"""mksym.py <out.pptx>: a deck of symbol fonts and Unicode glyphs."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkdecks import deck, xfrm, title
from xml.sax.saxutils import escape

def run(text, latin=None, sym=None, ea=None, cs=None, sz=2000):
    f = ""
    if latin: f += '<a:latin typeface="%s"/>' % latin
    if ea: f += '<a:ea typeface="%s"/>' % ea
    if cs: f += '<a:cs typeface="%s"/>' % cs
    if sym: f += '<a:sym typeface="%s"/>' % sym
    return '<a:r><a:rPr lang="en-US" sz="%d">%s</a:rPr><a:t>%s</a:t></a:r>' % (sz, f, escape(text))

def rows(i, items, x=40, y=100, w=880, h=420):
    ps = "".join('<a:p>%s%s</a:p>' % (run(lbl + "  ", sz=1400), r) for lbl, r in items)
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="t"/><p:cNvSpPr txBox="1"/><p:nvPr/></p:nvSpPr><p:spPr>%s<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr>'
            '<p:txBody><a:bodyPr wrap="square"/><a:lstStyle/>%s</p:txBody></p:sp>' % (i, xfrm(x, y, w, h), ps))

s1 = [
    ("Symbol, letters abgdpSW (PowerPoint: αβγδπΣΩ)", run("abgdpSW", latin="Symbol", sym="Symbol")),
    ("Symbol, PUA F061.. (αβγ)", run("", latin="Symbol", sym="Symbol")),
    ("a:sym only, Latin-1: drawn as is (ÒÓ³£)", run("ÒÓ³£", sym="Symbol")),
    ("Wingdings JKL (☺😐☹), ü (✓), Ø (➢)", run("JKL ü Ø ¨", latin="Wingdings", sym="Wingdings")),
    ("Wingdings PUA F0FC F0E0 F0D8", run("   ", latin="Wingdings", sym="Wingdings")),
    ("Wingdings 2: P O (✓✗)", run("P O Ï", latin="Wingdings 2", sym="Wingdings 2")),
    ("Wingdings 3: f g (arrows)", run("f g h i", latin="Wingdings 3", sym="Wingdings 3")),
    ("Webdings: a r (✓, ✕)", run("a r û", latin="Webdings", sym="Webdings")),
    ("MT Extra / Marlett (not on Mac)", run("abc", latin="Marlett", sym="Marlett")),
]
s2 = [
    ("Greek, math", run("αβγ ΔΩ ∑∫∮√∞ ≤≥≠≈ ±×÷ ∂∇ ∈∉⊂∪∩ ℏ ℝ ℤ 𝔽 𝒜")),
    ("Arrows, super/sub", run("← → ↔ ⇒ ⇔ ↑↓  x² y³ H₂O 10⁻⁹ ½ ¾ ‰ °C µm Ω")),
    ("Dingbats, shapes", run("✓ ✗ ✔ ✘ ★ ☆ ● ○ ■ □ ▲ ▼ ◆ ♠ ♣ ♥ ♦ ☎ ✉ ☀ ⚡ ⚠")),
    ("Emoji", run("😀 👍 👍🏽 🚀 ❤️ 🇩🇪 🇯🇵 👨‍👩‍👧 ✅ ❌")),
    ("CJK (ea MS Gothic)", run("日本語のテキスト 中文 한국어", ea="MS Gothic")),
    ("Arabic (cs), Hebrew", run("مرحبا بالعالم  שלום עולם", cs="Arial")),
    ("Devanagari, Thai", run("नमस्ते दुनिया  สวัสดี")),
    ("Combining, ligatures", run("é ä ñ  ﬁ ﬂ  office  Å Ø ß ŀ")),
    ("Box, technical, currency", run("┌─┬─┐ │ ⌘ ⌥ ⏎ ⎋ € £ ¥ ₹ ₿ № ™ ® ©")),
    ("Music, misc", run("♩ ♪ ♫ ♬ ☯ ☮ ⚛ ♻ ⚙ ⌀ ∅")),
]

def bul(i, items):
    ps = ""
    for font, ch, txt in items:
        bf = '<a:buFont typeface="%s"/>' % font if font else ""
        ps += ('<a:p><a:pPr marL="342900" indent="-342900">%s<a:buChar char="%s"/></a:pPr>%s</a:p>'
               % (bf, escape(ch), run(txt, sz=2000)))
    return ('<p:sp><p:nvSpPr><p:cNvPr id="%d" name="b"/><p:cNvSpPr txBox="1"/><p:nvPr/></p:nvSpPr><p:spPr>%s<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr>'
            '<p:txBody><a:bodyPr wrap="square"/><a:lstStyle/>%s</p:txBody></p:sp>' % (i, xfrm(40, 100, 880, 420), ps))

s3 = [("Wingdings", "§", "Wingdings § (▪)"), ("Wingdings", "Ø", "Wingdings Ø (➢)"), ("Wingdings", "ü", "Wingdings ü (✓)"),
      ("Wingdings", "q", "Wingdings q (❑)"), ("Wingdings", "v", "Wingdings v (❖)"), ("Wingdings", "", "Wingdings PUA F0D8 (➢)"),
      ("Symbol", "·", "Symbol · (•)"), ("Arial", "–", "Arial – (en dash)"), ("Courier New", "o", "Courier New o"),
      ("Wingdings 2", "P", "Wingdings 2 P (✓)"), ("", "✔", "no font, ✔"), ("Apple Color Emoji", "\U0001F449", "emoji 👉")]

slides = [(title(2, "Symbol fonts") + rows(3, s1), [], None),
          (title(2, "Unicode glyphs") + rows(3, s2), [], None),
          (title(2, "Bullets in symbol fonts") + bul(3, s3), [], None)]
deck(sys.argv[1], slides, {})

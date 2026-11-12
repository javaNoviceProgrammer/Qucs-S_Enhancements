# Writes klayout_look/: layouts, their .lyp files and the pictures KLayout
# draws of them, which test_layout_doc's KLayout look must match pixel for
# pixel (a micrometre a pixel, the picture's lower left at (0, 0)).
#   klayout -zz -r klayout_look.py        (KLayout 0.30.12 wrote these)
#
#   sample:   the boxes and .lyp the test's other KLayout-look checks use
#             (writeLypSample, kLypSample), on white and on black.
#   patterns: the 47 built-in patterns on boxes 40 µm wide, each at its
#             own place in the list (each place a row lower).
#   rules:    groups (what a group sets over its members', brightness
#             added, the wider frame, transparent), */* and */0 entries,
#             a member's */5 in a group of 1/*, a layer named twice, no
#             colour or pattern given, width 0, dashed frames 1 to 3 wide,
#             a file's own line style, crossed and marked boxes, paths'
#             spines and marks, a layer not valid, one hidden, layers named
#             by none - on white and on black.
import os
import pya

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "klayout_look")
os.makedirs(OUT, exist_ok=True)
U = 1000   # database units a µm


def box(cell, ly, l, d, x0, y0, x1, y1):
    cell.shapes(ly.layer(l, d)).insert(pya.Box(x0 * U, y0 * U, x1 * U, y1 * U))


def path(cell, ly, l, d, points, width):
    cell.shapes(ly.layer(l, d)).insert(pya.Path([pya.Point(x * U, y * U) for x, y in points], width * U))


def entry(source, **props):
    text = "".join("<%s>%s</%s>" % (k.replace("_", "-"), v, k.replace("_", "-")) for k, v in props.items() if k != "members")
    text += "<source>%s</source>" % source
    text += "".join("<group-members>%s</group-members>" % m for m in props.get("members", []))
    return text


def lyp(entries, extra=""):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<layer-properties>\n'
            + "".join(" <properties>%s</properties>\n" % e for e in entries) + extra + "</layer-properties>\n")


def picture(name, size, background):
    view = pya.LayoutView()
    view.set_config("background-color", background)
    view.set_config("grid-visible", "false")
    view.set_config("text-visible", "false")
    view.load_layout(os.path.join(OUT, name + ".gds"), True)
    view.load_layer_props(os.path.join(OUT, name + ".lyp"), True)
    view.max_hier()
    view.zoom_box(pya.DBox(0, 0, size, size))
    suffix = "" if background == "#ffffff" else "_dark"
    view.get_pixels(size, size).write_png(os.path.join(OUT, name + suffix + ".png"))


def write(name, ly, text, size, backgrounds=("#ffffff",)):
    ly.write(os.path.join(OUT, name + ".gds"))
    with open(os.path.join(OUT, name + ".lyp"), "w") as f:
        f.write(text)
    for b in backgrounds:
        picture(name, size, b)


# sample
ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")
for (x0, y0, x1, y1), l in [((0, 0, 40, 40), 1), ((20, 20, 60, 60), 2), ((10, 30, 50, 70), 3), ((70, 70, 90, 90), 4),
                            ((70, 10, 90, 30), 5), ((70, 40, 90, 60), 6), ((60, 45, 80, 55), 7), ((92, 2, 98, 8), 8)]:
    box(top, ly, l, 0, x0, y0, x1, y1)
write("sample", ly, lyp([
    entry("2/0@1", frame_color="#0000ff", fill_color="#0000ff", dither_pattern="I0", name="M2"),
    entry("1/0@1", frame_color="#ff0000", fill_color="#ff0000", dither_pattern="C0", name="M1"),
    entry("*/*@*", name="FEOL", members=[
        entry("3/0", frame_color="#000080", fill_color="#00a000", dither_pattern="I4", line_style="I2", name="POLY"),
        entry("4/0", fill_color="#ff00ff", dither_pattern="I0", visible="false", name="HIDDEN")]),
    entry("5/0@1", frame_color="#000000", fill_color="#ff0000", dither_pattern="I1", width="3", xfill="true", marked="true", name="BOX"),
    entry("7/0@1", frame_color="#ff8040", fill_color="#ff8040", dither_pattern="I0", name="UNDER"),
    entry("6/0@1", frame_color="#80ff20", fill_color="#80ff20", dither_pattern="I0", transparent="true", name="OVER")],
    " <custom-dither-pattern><pattern><line>**..</line><line>*...</line><line>....</line></pattern>"
    "<order>1</order><name>mine</name></custom-dither-pattern>\n"), 100, ("#ffffff", "#000000"))

# patterns
ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")
for i in range(47):
    x, y = (i % 8) * 50 + 5, 300 - (i // 8) * 50 + 5
    box(top, ly, 100 + i, 0, x, y, x + 40, y + 40)
write("patterns", ly, lyp([entry("%d/0@1" % (100 + i), frame_color="#000000", fill_color="#000000", dither_pattern="I%d" % i)
                           for i in range(47)]), 400)

# rules
ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")
for l, d, x0, y0, x1, y1 in [(1, 0, 5, 165, 35, 195), (2, 0, 25, 155, 65, 190), (3, 0, 85, 165, 115, 195), (1, 5, 125, 165, 155, 195),
                             (14, 0, 165, 165, 195, 195), (4, 0, 5, 125, 55, 155), (5, 0, 65, 125, 95, 155), (6, 0, 105, 125, 135, 155),
                             (7, 0, 145, 125, 175, 155), (8, 0, 5, 85, 35, 115), (11, 0, 45, 85, 75, 115), (12, 0, 85, 85, 115, 115),
                             (13, 0, 125, 85, 155, 115), (15, 0, 165, 85, 195, 115), (0, 0, 5, 5, 35, 35), (20, 0, 45, 5, 75, 35),
                             (21, 3, 85, 5, 115, 35), (37, 0, 125, 5, 155, 35), (16, 0, 165, 5, 195, 35)]:
    box(top, ly, l, d, x0, y0, x1, y1)
path(top, ly, 9, 0, [(10, 45), (60, 45), (60, 70)], 6)
path(top, ly, 10, 0, [(80, 55), (150, 55)], 4)
write("rules", ly, lyp([
    entry("*/*@*", name="G", fill_color="#ff0000", dither_pattern="I4", width="3", fill_brightness="40", transparent="true", members=[
        entry("1/0@1", frame_color="#0000ff", fill_color="#0000ff", dither_pattern="I8", width="1", fill_brightness="-20"),
        entry("2/0@1", frame_color="#008000")]),
    entry("*/*@*", frame_color="#00ffff", fill_color="#00ffff", dither_pattern="I0"),
    entry("*/*@*", name="OUT", frame_color="#000080", line_style="I2", members=[
        entry("*/*@*", name="IN", frame_color="#00ff00", fill_color="#804000", dither_pattern="I12", members=[entry("3/0@1")])]),
    entry("*/0@1", frame_color="#ff00ff", fill_color="#ff00ff", dither_pattern="I0"),
    entry("1/*@1", fill_color="#008080", dither_pattern="I16", members=[entry("*/5", fill_color="#ff8000")]),
    entry("14/0@1"),
    entry("4/0@1", frame_color="#c08000", fill_color="#c08000", dither_pattern="I17"),
    entry("5/0@1", frame_color="#000000", fill_color="#808080", dither_pattern="I0", width="0"),
    entry("6/0@1", frame_color="#a000a0", fill_color="#a000a0", dither_pattern="I1", line_style="I3", width="2"),
    entry("7/0@1", frame_color="#00a0a0", fill_color="#00a0a0", dither_pattern="I32", line_style="C0", width="1"),
    entry("8/0@1", frame_color="#404040", fill_color="#404040", dither_pattern="I1", line_style="I7", width="3"),
    entry("9/0@1", frame_color="#0060ff", fill_color="#0060ff", dither_pattern="I9", line_style="I2", marked="true"),
    entry("10/0@1", frame_color="#ff6000", fill_color="#ff6000", dither_pattern="I13", width="2", marked="true"),
    entry("11/0@1", frame_color="#000000", fill_color="#ff0000", dither_pattern="I1", width="2", xfill="true", marked="true"),
    entry("12/0@1", frame_color="#00c000", fill_color="#00c000", dither_pattern="I23", valid="false"),
    entry("13/0@1", frame_color="#ff0000", fill_color="#ff0000", visible="false"),
    entry("4/0@1", frame_color="#0000c0", fill_color="#0000c0", dither_pattern="I30"),
    entry("16/0@1", frame_color="#800080", fill_color="#800080", dither_pattern="I1", line_style="I4")],
    " <custom-line-style><pattern>**.*..</pattern><order>1</order><name>mine</name></custom-line-style>\n"),
    200, ("#ffffff", "#000000"))
print("written to", OUT)

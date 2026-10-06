# Writes klayout_sample.oas, the layout test_layout_doc reads as another
# program's OASIS: KLayout 0.30.8's writer, compressed blocks, strict mode,
# its own repetitions (the 400 squares on 4/0), a property and a layer name.
#   klayout -b -r klayout_sample.py
# It prints what a flat view of CHIP has on each layer, which the test
# expects.
import pya
ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("CHIP")
inv = ly.create_cell("INV")
via = ly.create_cell("VIA1")
m1 = ly.layer(pya.LayerInfo(1, 0, "Metal1"))
m2 = ly.layer(2, 0)
v1 = ly.layer(3, 0)
m4 = ly.layer(4, 0)
p5 = ly.layer(5, 0)
txt = ly.layer(10, 0)
body = inv.shapes(m1).insert(pya.Box(0, 0, 2000, 1000))
body.set_property("net", "A")
inv.shapes(m2).insert(pya.Path([pya.Point(0, 500), pya.Point(2000, 500)], 200))
inv.shapes(txt).insert(pya.Text("OUT", 2000, 500))
via.shapes(v1).insert(pya.Box(0, 0, 500, 500))
top.insert(pya.CellInstArray(inv.cell_index(), pya.Trans(pya.Point(10000, 0))))
top.insert(pya.CellInstArray(inv.cell_index(), pya.Trans(pya.Trans.R90, pya.Vector(20000, 0))))
top.insert(pya.CellInstArray(inv.cell_index(), pya.Trans(pya.Trans.M0, pya.Vector(30000, 0))))
top.insert(pya.CellInstArray(via.cell_index(), pya.Trans(), pya.Vector(1000, 0), pya.Vector(0, 1000), 3, 2))
for i in range(20):
    for j in range(20):
        top.shapes(m4).insert(pya.Box(i * 1000, 20000 + j * 1000, i * 1000 + 200, 20200 + j * 1000))
top.shapes(p5).insert(pya.Polygon([pya.Point(0, 10000), pya.Point(4000, 10000), pya.Point(2000, 13000)]))
o = pya.SaveLayoutOptions()
o.format = "OASIS"
o.oasis_compression_level = 10
o.oasis_write_cblocks = True
o.oasis_strict_mode = True
o.oasis_write_std_properties = 1
ly.write("klayout_sample.oas", o)
g = pya.SaveLayoutOptions()
g.format = "GDS2"
ly.write("klayout_sample.gds", g)
# What a flat view has, per layer, for the test.
counts = {}
for li in ly.layer_indexes():
    info = ly.get_info(li)
    n = 0
    it = top.begin_shapes_rec(li)
    while not it.at_end():
        n += 1
        it.next()
    counts["%d/%d" % (info.layer, info.datatype)] = n
print(sorted(counts.items()))
print("bbox", top.dbbox())

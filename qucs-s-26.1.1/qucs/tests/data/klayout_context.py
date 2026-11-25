# Writes klayout_context.gds: a layout as KLayout's writer leaves one (and
# gdsfactory's, which writes through it) - TOP, a box and a placed PCell of
# KLayout's Basic library, and the $$$CONTEXT_INFO$$$ cell KLayout adds to
# keep that PCell's library and parameters (KLayout reads it and does not
# show it; test_layout_doc expects the viewer not to either). And
# klayout_context_many.gds: TOP placing one cell that places five PCells -
# KLayout's context cell then has five references, TOP one: the cell
# edit_layout, the router and the multiphysics import must still take for
# the top. And klayout_context_pcell.gds: a PCell placed by no cell, its
# own layout's top cell, which only the context cell refers to. And
# klayout_context_library.gds: a library's cell BLOCK, placing its PART,
# taken into a layout and placed by no cell - its top cell, PART written
# before it.
#   klayout -zz -r klayout_context.py
import os
import pya
ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")
top.shapes(ly.layer(1, 0)).insert(pya.Box(0, 0, 10000, 5000))
text = ly.create_cell("TEXT", "Basic", {"text": "HI", "layer": pya.LayerInfo(2, 0), "mag": 2.0})
top.insert(pya.CellInstArray(text.cell_index(), pya.Trans()))
ly.write(os.path.join(os.path.dirname(os.path.abspath(__file__)), "klayout_context.gds"))

ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")
block = ly.create_cell("BLOCK")
top.insert(pya.CellInstArray(block.cell_index(), pya.Trans()))
for i in range(5):
    t = ly.create_cell("TEXT", "Basic", {"text": "T%d" % i, "layer": pya.LayerInfo(2, 0), "mag": 1.0})
    block.insert(pya.CellInstArray(t.cell_index(), pya.Trans(pya.Vector(0, i * 2000))))
ly.write(os.path.join(os.path.dirname(os.path.abspath(__file__)), "klayout_context_many.gds"))

ly = pya.Layout()
ly.dbu = 0.001
ly.create_cell("TEXT", "Basic", {"text": "TOP", "layer": pya.LayerInfo(3, 0), "mag": 1.0})
ly.write(os.path.join(os.path.dirname(os.path.abspath(__file__)), "klayout_context_pcell.gds"))


class ProbeLibrary(pya.Library):
    def __init__(self):
        self.description = "the test's library"
        lib = self.layout()
        lib.dbu = 0.001
        part = lib.create_cell("PART")
        part.shapes(lib.layer(1, 0)).insert(pya.Box(0, 0, 1000, 1000))
        block = lib.create_cell("BLOCK")
        block.insert(pya.CellInstArray(part.cell_index(), pya.Trans(pya.Vector(2000, 0))))
        block.shapes(lib.layer(2, 0)).insert(pya.Box(0, 0, 5000, 3000))
        self.register("ProbeLib")


ProbeLibrary()
ly = pya.Layout()
ly.dbu = 0.001
ly.create_cell("BLOCK", "ProbeLib")
ly.write(os.path.join(os.path.dirname(os.path.abspath(__file__)), "klayout_context_library.gds"))

# Writes klayout_context.gds: a layout as KLayout's writer leaves one (and
# gdsfactory's, which writes through it) - TOP, a box and a placed PCell of
# KLayout's Basic library, and the $$$CONTEXT_INFO$$$ cell KLayout adds to
# keep that PCell's library and parameters (KLayout reads it and does not
# show it; test_layout_doc expects the viewer not to either).
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

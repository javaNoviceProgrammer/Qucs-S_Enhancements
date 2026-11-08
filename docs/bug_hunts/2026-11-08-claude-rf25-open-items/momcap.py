"""The MOM comb of the 6 Nov reports: 12 fingers, W = S = 1 um, 40 um overlap, 1 um end gap, 3 um buses;
oxide eps_r 4.1 from a perfect plane up to 8 um, air above, metal bottom at 4 um; lossless metal; a port from
the plane to each bus's outer face."""
import sys; sys.path.insert(0, "."); from mk import *
def comb(out, name, t, mmodel, cells, fingers=12, overlap=40, **study):
    W = S = 1.0; gapEnd = 1.0; bus = 3.0
    width = fingers * W + (fingers - 1) * S
    shapes = [(1, (0, -bus, width, 0)), (1, (0, overlap + 2 * gapEnd, width, overlap + 2 * gapEnd + bus))]
    for i in range(fingers):
        x0 = i * (W + S)
        shapes.append((1, (x0, 0, x0 + W, overlap + gapEnd)) if i % 2 == 0 else (1, (x0, gapEnd, x0 + W, overlap + 2 * gapEnd)))
    stack = [node("rf_ground", "gnd", "Ground", {"kind": "perfect"}),
             node("rf_substrate", "sub", "Oxide", {"thickness": "4[um]", "epsr": "4.1", "tand": "0", "sigma": "0"}),
             node("rf_metal", "m1", "M", {"thickness": f"{t}[um]", "sigma": "1e12", "model": mmodel}),
             node("rf_dielectric", "ild", "Oxide2", {"thickness": f"{4 - t}[um]", "epsr": "4.1", "tand": "0"}),
             node("rf_cover", "cov", "Cover", {"kind": "open"})]
    top = overlap + 2 * gapEnd + bus
    ports = [edge("p1", "M", width / 2, -bus, "-y"), edge("p2", "M", width / 2, top, "+y")]
    st = {"from": "1[GHz]", "to": "100[GHz]", "points": "34", "matrix": "automatic"}; st.update(study)
    model(f"{out}/{name}.qrf25", shapes, stack, [(1, 0, "M")], ports, cells=cells, study=st)
if __name__ == "__main__":
    out = sys.argv[1]
    for c in (3, 6, 8, 10, 12):
        comb(out, f"thin_c{c}", 0, "sheet", c)
        comb(out, f"thick_faces_c{c}", 1, "faces", c)
        comb(out, f"thick_sheet_c{c}", 1, "sheet", c)
        comb(out, f"thin01_faces_c{c}", 0.01, "faces", c)

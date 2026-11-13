"""Reproducer models for the RF 2.5D open items: GDSII layouts and .qrf25 models."""
import json, os, struct

def gds(path, shapes, cell="TOP"):
    """shapes: list of (layer, [(x,y),...]) in um, or (layer, (x0,y0,x1,y1)) rectangles."""
    def rec(rt, dt, data=b""):
        return struct.pack(">HBB", 4 + len(data), rt, dt) + data
    def real8(x):
        if x == 0: return b"\0" * 8
        s = 0x80 if x < 0 else 0; x = abs(x); e = 64
        while x >= 1: x /= 16; e += 1
        while x < 1 / 16: x *= 16; e -= 1
        return struct.pack(">B", s | e) + int(x * 2 ** 56).to_bytes(7, "big")
    ts = struct.pack(">12h", 2026, 11, 8, 0, 0, 0, 2026, 11, 8, 0, 0, 0)
    out = rec(0x00, 0x02, struct.pack(">h", 600)) + rec(0x01, 0x02, ts) + rec(0x02, 0x06, b"LIB\0")
    out += rec(0x03, 0x05, real8(1e-3) + real8(1e-9))
    out += rec(0x05, 0x02, ts) + rec(0x06, 0x06, cell.encode() + (b"\0" if len(cell) % 2 else b""))
    for layer, s in shapes:
        if len(s) == 4 and not isinstance(s[0], (tuple, list)):
            x0, y0, x1, y1 = s; s = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
        p = list(s) + [s[0]]
        xy = b"".join(struct.pack(">ii", round(x * 1000), round(y * 1000)) for x, y in p)
        out += rec(0x08, 0x00) + rec(0x0D, 0x02, struct.pack(">h", layer)) + rec(0x0E, 0x02, struct.pack(">h", 0))
        out += rec(0x10, 0x03, xy) + rec(0x11, 0x00)
    out += rec(0x07, 0x00) + rec(0x04, 0x00)
    open(path, "wb").write(out)

def node(t, tag, label=None, props=None, children=None):
    n = {"type": t, "tag": tag, "properties": props or {}}
    if label: n["label"] = label
    if children is not None: n["children"] = children
    return n

def model(path, shapes, stack, layermap, ports, cells=6, study=None, mesh=None, desc="", processing=None):
    """stack: list of nodes; layermap: [[gdslayer, 0, stacklayer]]; ports: list of nodes."""
    base = os.path.splitext(path)[0]
    gds(base + ".gds", shapes)
    m = {"cells_across": str(cells)}
    if mesh: m.update(mesh)
    st = {"kind": "full_wave", "from": "1[GHz]", "to": "10[GHz]", "points": "10", "matrix": "automatic"}
    if study: st.update(study)
    tree = node("rf_model", "model", "Model", {"description": desc}, [
        node("rf_parameters", "par", "Parameters", {}, []),
        node("rf_stack", "stack", "Stack", {}, stack),
        node("rf_layout", "lay", "Layout", {"file": os.path.basename(base) + ".gds", "cell": "TOP", "unit": "1[um]"}, [
            node("rf_layer_map", "map", "Layer map", {"rows": [[str(a), str(b), c] for a, b, c in layermap]}, []),
            node("rf_processing", "proc", "Processing", processing or {}, [])]),
        node("rf_ports", "ports", "Ports", {}, ports),
        node("rf_mesh", "mesh", "Mesh", m, []),
        node("rf_study", "study", "Study", st, []),
        node("rf_results", "res", "Results", {}, []),
        node("rf_exports", "exp", "Exports", {}, [])])
    json.dump({"format": "qucs-s rf 2.5d 1", "tree": tree}, open(path, "w"), indent=1)

def edge(tag, cond, x, y, facing, ret="ground", calibrate=False, **kw):
    p = {"conductor": cond, "x": f"{x}[um]", "y": f"{y}[um]", "facing": facing, "return": ret, "calibrate": calibrate}
    p.update(kw)
    return node("rf_edge_port", tag, tag.upper(), p)

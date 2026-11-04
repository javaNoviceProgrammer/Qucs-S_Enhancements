"""The feature request's acceptance structures, made from copies of the reviewer's GSG pad models
(rf25_zotero_benchmarks/gsg_l2l, make_gsg_l2l.py): CPW S 50 / G 20 um on 500 um quartz, nothing below, 2 um Au,
80 um pads 60 um long, 80 um tapers, lines of 0.5 and 1 mm.

- edge_L*: the reviewer's coplanar edge ports (return beside), as given.
- tips_L*: an internal port on a 20 x 20 um patch at each signal pad's centre (30 um in from its outer edge),
  returning through two 20 x 20 um terminals at the ground pads' centres (|y| = 137.5 um), tied.
- tips6_L*, edge6_L*: the tips and the edge ports at 6 cells across, held compressed (the request's seventh test).
- gs_L*: one ground tip only (the upper), a ground-signal probe.
- skew_L*: the lower ground tip moved to |y| = 180 um, off its pad's centre.
- bridge{20,10,5}_L*: the reviewer's probe-bridge workaround with the bridge that high (um).
Run from this folder: python3 -B -I make_models.py <models folder>."""
import copy, json, os, sys

out = sys.argv[1]
LP, LT, TIP, PX = 60, 80, 20, 30          # pad length, taper length, tip side, tip x in from the pad's outer edge
YT = (80 / 2 + 30 + 205) / 2              # ground tips at the ground pads' middle: 137.5 um


def load(name):
    return json.load(open(os.path.join(out, name)))


def save(model, name):
    json.dump(model, open(os.path.join(out, name), "w"), indent=2)


def child(model, kind):
    return next(c for c in model["tree"]["children"] if c["type"] == kind)


def node(t, tag, label, **props):
    return {"type": t, "tag": tag, "label": label, "properties": props}


def square(x, y):
    return {"x": f"{x}[um]", "y": f"{y}[um]", "width": f"{TIP}[um]", "height": f"{TIP}[um]"}


def tips(model, e, lower=-YT, both=True):
    ports = []
    for i, x in enumerate((PX, e - PX)):
        names = [f"T{2 * i + 1}"] + ([f"T{2 * i + 2}"] if both else [])
        ports.append(node("rf_internal_port", f"p{i + 1}", f"P{i + 1}", to="Au", **{"return": "terminals"}, returns=names,
                          **square(x, 0)))
    for i, x in enumerate((PX, e - PX)):
        ports.append(node("rf_terminal", f"t{2 * i + 1}", f"T{2 * i + 1}", metal="Au", **square(x, YT)))
        if both:
            ports.append(node("rf_terminal", f"t{2 * i + 2}", f"T{2 * i + 2}", metal="Au", **square(x, lower if i == 0 else -YT)))
    child(model, "rf_ports")["children"] = ports
    return model


for L in (500, 1000):
    e = L + 2 * (LP + LT)
    edge = load(f"gsg_pads_L{L}.qrf25")
    save(edge, f"edge_L{L}.qrf25")
    save(tips(copy.deepcopy(edge), e), f"tips_L{L}.qrf25")
    six = tips(copy.deepcopy(edge), e)
    child(six, "rf_mesh")["properties"]["cells_across"] = "6"
    child(six, "rf_study")["properties"]["matrix"] = "compressed"
    save(six, f"tips6_L{L}.qrf25")
    edge6 = copy.deepcopy(edge)
    child(edge6, "rf_mesh")["properties"]["cells_across"] = "6"
    child(edge6, "rf_study")["properties"]["matrix"] = "compressed"
    save(edge6, f"edge6_L{L}.qrf25")
    save(tips(copy.deepcopy(edge), e, both=False), f"gs_L{L}.qrf25")
    save(tips(copy.deepcopy(edge), e, lower=-180), f"skew_L{L}.qrf25")
    probe = load(f"gsg_probe_L{L}.qrf25")
    for h in (20, 10, 5):
        b = copy.deepcopy(probe)
        for layer in child(b, "rf_stack")["children"]:
            if layer["label"] == "ProbeAir":
                layer["properties"]["thickness"] = f"{h}[um]"
        save(b, f"bridge{h}_L{L}.qrf25")
# (The layouts: the edge and tip models share the pads' file, the bridges the probe's.)
print("ok")

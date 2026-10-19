import json, sys
out, points, hmax = sys.argv[1], sys.argv[2], sys.argv[3]
m = {"format": "qucs-s multiphysics 1",
  "parameters": [{"name": "Va", "expression": "0[V]"}],
  "components": [{"unit": "um",
    "geometry": [{"type": "rectangle", "label": "pplus", "position": ["-0.5", "0"], "size": ["0.5", "0.2"]},
                 {"type": "rectangle", "label": "nside", "position": ["0", "0"], "size": ["4", "0.2"]}],
    "materials": [{"material": "Silicon", "selection": {"all": True}}],
    "physics": [{"type": "semiconductor", "tag": "semi", "features": [
      {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
      {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"only": ["nside"]}},
      {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "left"}},
      {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}},
      {"type": "impact", "selection": {"all": True}},
      {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "selection": {"all": True}}]}],
    "mesh": {"size": "normal", "custom": True, "hmax": hmax}}],
  "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", points, "V"]]}, {"type": "stationary"}]}]}
json.dump(m, open(out, 'w'), indent=1)

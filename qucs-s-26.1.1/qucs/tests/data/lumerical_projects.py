# Writes lumerical_charge.ldev and lumerical_mode.lms: small files laid out
# as Lumerical's CHARGE projects (the Finite Element IDE's .ldev, HDF5) and
# MODE projects (.lms, its own binary) are, for test_fem_lumerical's
# importers.
#   python3 lumerical_projects.py        (h5py 3, NumPy)
# The .ldev: its materials' records (SemiconductorMaterials/Materials, each
# its name, kind, permittivity, work function), its objects' "Properties"
# blobs (keys "<name>_value"): a geometry group of rectangles and a polygon
# turned about x into the x-z plane, a 2D y-normal simulation region, the
# CHARGE solver (its blob after a 16-byte class id) with constant and
# diffusion doping (per cm3, as Lumerical keeps them), a mesh constraint and
# its contacts. The .lms: its material section (a sampled permittivity -
# real parts, then imaginary ones -, a dielectric, an object defined one),
# then its object tree: a structure's box and index before its properties,
# a solver's properties, each its children.
# The .lms besides (MODE 2020's): Sellmeier and temperature dependent
# materials; a pyramid (its spans at its bottom and top); a structure of a
# kind not read; a structure group (all its values tagged, its script and
# own properties after them) with a polygon and a rectangle its script
# made, relative to the group's place; each structure's tagged tail, its
# mesh order flag (0 its own order, 1 its material's).
# lumerical_device.ldev: DEVICE 7.17's layout - its materials the project's
# material objects, each record under its children's Custom/1/<id> with
# its models (a group each: "Active Model", "Name", "Enabled", "Carrier
# Type" 110 n / 112 p; each model's parameters a child group's
# attributes) -, a pyramid, a diffusion box as wide as its junction, surface
# recombination, an imported optical generation ("Data" blobs, both
# layouts), a monitor. lumerical_heat.ldev: a HEAT project - its materials'
# thermal records (Solid, Fluid), a temperature, a convection, a heat source.
import struct
import numpy as np, h5py

def le(fmt, *v): return struct.pack('<' + fmt, *v)

def entry(k, v):
    key = k.encode() + b'\0'
    out = le('I', len(key)) + key
    if isinstance(v, str):
        s = v.encode() + b'\0'
        return out + le('II', 0, len(s)) + s
    if isinstance(v, bool) or isinstance(v, int):
        return out + le('III', 2, 1, int(v))
    if isinstance(v, tuple) and v[0] == 'ints':
        return out + le('II', 2, len(v[1])) + b''.join(le('i', x) for x in v[1])
    if isinstance(v, tuple) and v[0] == 'matrix':   # (rows, cols, column-major values)
        _, rows, cols, values = v
        return out + le('IIII', 3, 0, rows * cols, 2) + le('II', rows, cols) + le('II', 1, 1) + b''.join(le('d', x) for x in values)
    if isinstance(v, tuple) and v[0] == 'complex':  # a column: real parts, then imaginary ones
        _, re, im = v
        return out + le('IIII', 3, 0, len(re), 2) + le('II', len(re), 1) + le('II', 0, 1) + b''.join(le('d', x) for x in re) + b''.join(le('d', x) for x in im)
    return out + le('IId', 1, 1, float(v))

def blob(entries):
    return le('I', len(entries)) + b''.join(entry(k, v) for k, v in entries.items())

def props(group, entries, prefix=b''):
    group.create_dataset('Properties', data=np.frombuffer(prefix + blob({k + '_value': v for k, v in entries.items()}), dtype=np.int8))

# --- CHARGE ---------------------------------------------------------------
ids = {'Si (Silicon)': '{11111111-0000-4000-8000-000000000001}', 'SiO2 (Glass) - Sze': '{11111111-0000-4000-8000-000000000002}',
       'Al (Aluminium) - CRC': '{11111111-0000-4000-8000-000000000003}', 'p-poly gate (sample)': '{11111111-0000-4000-8000-000000000004}',
       'HfO2 (Hafnium oxide) - Robertson': '{11111111-0000-4000-8000-000000000005}'}
kinds = {'Si (Silicon)': ('Semiconductor', 11.7, 4.2), 'SiO2 (Glass) - Sze': ('Insulator', 3.9, 0.0), 'Al (Aluminium) - CRC': ('Conductor', 1.0, 4.28),
         'p-poly gate (sample)': ('Conductor', 1.0, 5.17), 'HfO2 (Hafnium oxide) - Robertson': ('Insulator', 25.0, 0.0)}
with h5py.File('lumerical_charge.ldev', 'w', libver='latest') as f:
    f.create_group('FileVersionInfo/DEVICE').attrs['version'] = np.bytes_('7.1')
    for name, uid in ids.items():
        rec = f.create_group('SemiconductorMaterials/Materials/' + uid)
        kind, eps, wf = kinds[name]
        for k, v in {'name': np.bytes_(name), 'type': np.bytes_('Electrical Material'), 'epsilonr': np.float64(eps), 'workfunction': np.float64(wf),
                     'mesh order': np.int32(2)}.items():
            rec.attrs[k] = v
        rec.create_group('Detail').attrs['model label'] = np.bytes_(kind)
    # Its thermal database beside it: the oxide's record under the same id,
    # its conductivity of T.
    rec = f.create_group('ThermalMaterials/Materials/' + ids['SiO2 (Glass) - Sze'])
    rec.attrs['name'] = np.bytes_('SiO2 (Glass) - Sze')
    for role, active, params in [('Thermal Conductivity', 'Universal Temperature', {'A': 1.4, 'eta': 0.1}), ('Mass Density', 'Constant', {'A0': 2200.0}),
                                 ('Specific Heat', 'Constant', {'A0': 730.0})]:
        g = rec.create_group(role)
        for k, v in {'Active Model': np.bytes_(active), 'Name': np.bytes_(role), 'Enabled': np.int32(1)}.items():
            g.attrs[k] = v
        a = g.create_group(active)
        for k, v in params.items():
            a.attrs[k] = np.float64(v)
    root = f.create_group('SimObjectRoot')
    props(root, {'name': 'model', 'type': ''})
    mats = root.create_group('Children/1')
    props(mats, {'name': 'materials', 'type': 'Material Group'})
    for i, name in enumerate(ids):
        props(mats.create_group('Children/%d' % (i + 1)), {'name': name, 'type': 'Material Object'})
    geo = root.create_group('Children/2')
    props(geo, {'name': 'geometry', 'type': 'Geometry Group'})
    def rect(i, name, material, x0, x1, z0, z1, order):
        props(geo.create_group('Children/%d' % i), {'name': name, 'type': 'Rectangle', 'material': ids[material], 'mesh order': float(order),
                                                    'x min': x0, 'x max': x1, 'y min': -1e-6, 'y max': 1e-6, 'z min': z0, 'z max': z1,
                                                    'first axis': 0, 'rotation 1': 0.0})
    rect(1, 'silicon', 'Si (Silicon)', -3e-6, 3e-6, -2e-6, 0.0, 3)          # (wider than the region: clipped)
    rect(2, 'oxide', 'SiO2 (Glass) - Sze', -2e-6, 2e-6, 0.0, 2e-8, 3)
    rect(3, 'source', 'Al (Aluminium) - CRC', -2e-6, -1e-6, -5e-8, 2e-7, 2)    # (into the silicon: it wins there)
    rect(4, 'gate', 'p-poly gate (sample)', -5e-7, 5e-7, 2e-8, 1.2e-7, 2)
    rect(5, 'body', 'Al (Aluminium) - CRC', -3e-6, 3e-6, -2.3e-6, -2e-6, 2)   # (thick: kept, its contact between it and the silicon)
    # A spacer: a polygon in its own x-y plane, turned about x by 90° (its
    # y the global z), extruded along its z (the global -y).
    props(geo.create_group('Children/6'), {'name': 'spacer', 'type': 'Polygon', 'material': ids['HfO2 (Hafnium oxide) - Robertson'], 'mesh order': 2.0,
                                           'x': 0.0, 'y': 0.0, 'z': 0.0, 'z min': -1e-6, 'z max': 1e-6,
                                           'vertices': ('matrix', 3, 2, [5e-7, 8e-7, 5e-7, 2e-8, 2e-8, 1.2e-7]),
                                           'first axis': 0, 'second axis': 0, 'third axis': 1, 'rotation 3': 90.0})
    props(root.create_group('Children/3'), {'name': 'CHARGE simulation region', 'type': 'Simulation Region', 'dimension': 1,
                                            'x min': -2e-6, 'x max': 2e-6, 'y min': 0.0, 'y max': 0.0, 'z min': -2.3e-6, 'z max': 2e-7})
    solver = f.create_group('SemiconductorSimObject')
    props(solver, {'name': 'CHARGE', 'type': 'Solver', 'norm length': 2e-6, 'simulation temperature': 320.0}, prefix=bytes(range(16)))
    def child(i, entries, parent=solver):
        g = parent.create_group('Children/%d' % i)
        props(g, entries)
        return g
    child(1, {'name': 'pwell', 'type': 'Constant Doping Region', 'concentration': 1e17, 'dopant type': 1.0, 'volume type': 2, 'volume solid': '',
              'x min': -3e-6, 'x max': 3e-6, 'y min': -1e-6, 'y max': 1e-6, 'z min': -2.5e-6, 'z max': 1e-7})
    child(2, {'name': 'source n+', 'type': 'Diffusion Doping', 'concentration': 1e20, 'dopant type': 0.0, 'junction width': 1e-7,
              'ref concentration': 1e15, 'source face': 5, 'distribution function': 1, 'volume type': 2, 'volume solid': '',
              'x min': -2.2e-6, 'x max': -6e-7, 'y min': -1e-6, 'y max': 1e-6, 'z min': -3e-7, 'z max': 0.0})
    child(3, {'name': 'gatemesh', 'type': 'Electrical Mesh Constraint', 'enabled': 1, 'max edge length': 5e-9, 'volume type': 0,
              'x min': -1e-6, 'x max': 1e-6, 'y min': -1e-6, 'y max': 1e-6, 'z min': -1e-8, 'z max': 3e-8})
    bcs = child(4, {'name': 'boundary conditions', 'type': 'Boundary Condition Group'})
    child(1, {'name': 'source', 'type': 'Electrical Boundary Condition', 'solid': 'source', 'surface type': 3, 'sweep type': 0, 'voltage': 0.0,
              'force ohmic': 1, 'bc mode': 0}, bcs)
    child(2, {'name': 'gate', 'type': 'Electrical Boundary Condition', 'solid': 'gate', 'surface type': 3, 'sweep type': 1, 'voltage': 0.0,
              'range start': -1.0, 'range stop': 1.0, 'range num points': 5, 'force ohmic': 0, 'bc mode': 0}, bcs)
    child(3, {'name': 'body', 'type': 'Electrical Boundary Condition', 'solid': 'body', 'surface type': 3, 'sweep type': 0, 'voltage': 0.0,
              'force ohmic': 1, 'bc mode': 0}, bcs)
    child(4, {'name': 'surface_recombination', 'type': 'Surface Recombination', 'surface type': 6, 'solid': ''}, bcs)

# --- MODE -------------------------------------------------------------------
def guid(i): return ('{22222222-0000-4000-8000-%012d}' % i).encode()
def text(s):
    b = s.encode() + b'\0'
    return le('I', len(b)) + b
def solver(i, entries, children, extra=0):
    return le('II', 1001, 38) + guid(i) + blob(entries) + le('II', extra, len(children)) + b''.join(children)
# Tagged values: 0 an int, 1 a number, 2 a text, 3 a matrix (its name,
# elements, dimensions, real, its values).
def tint(v): return b'\x00' + le('i', v)
def tnum(v): return b'\x01' + le('d', v)
def ttext(s):
    b = s.encode() + b'\0'
    return b'\x02' + le('I', len(b)) + b
def tmat(dims, values, name=''):
    n = name.encode() + b'\0' if name else b''
    return b'\x03' + le('I', len(n)) + n + le('II', len(values), len(dims)) + b''.join(le('I', d) for d in dims) + le('II', 1, 1) + b''.join(le('d', x) for x in values)
# A structure's tail: its rotations (none), its display, its mesh order
# (None: its material's), a matrix after it.
def tail(order=None, after=None):
    out = tint(-1) * 3 + tmat([3, 1], [0, 0, 0]) + tint(0) + tint(0) + tnum(1.0)
    out += (tint(1) + tint(2)) if order is None else (tint(0) + tint(order))
    return out + (after if after is not None else b'') + tmat([1, 1], [0.5]) + tint(1) + tint(1)
def props_dict(material):
    return blob({'acf_custom': '0.5', 'acf_fixed': 0.5, 'backgroundId': '{00000000-0000-0000-0000-000000000000}', 'materialuuid': material,
                 'use_relative_coordinates': 1})
def structure(i, name, index, box, material, shape=6, order=None, inner=False):
    # (A rectangle's box: index, y min, x min, y max, x max, z min, z max; a
    # pyramid's: index, x, y, its x and y spans at the bottom, at the top,
    # z min, z max.)
    head = b'' if inner else le('II', 1000, 38) + guid(i)
    out = head + le('III', shape, 32 if shape == 6 else 26, 7) + le('%dd' % len(box), *box) + text(index if isinstance(index, str) else '%g' % index)
    out += le('II', 1, 0) + text(name) + tail(order) + props_dict(material)
    return out + le('I', 0)
def rect(i, name, index, box, material, order=None, inner=False):
    y0, x0, y1, x1, z0, z1 = box
    return structure(i, name, index, (float(index) if not isinstance(index, str) else 1.0, y0, x0, y1, x1, z0, z1), material, 6, order, inner)
def polygon(name, index, vertices, z0, z1, place, order=None):
    # (Inside a group: no kind and id; its values tagged, its dictionary, no
    # count of children.)
    vtx = [c for v in vertices for c in v]
    out = le('II', 11, 22) + tint(-1) + tnum(float(index.split(';')[0])) + tmat([2, len(vertices)], vtx, 'vtx') + tnum(z0) + tnum(z1) + ttext(index)
    out += tint(1) + tint(0) + ttext(name) + tail(order, tmat([2, 1], list(place)))
    return out + blob({'acf_custom': '0.5', 'gridAttributeName': '', 'materialuuid': none, 'use_relative_coordinates': 1})
def group(i, name, place, script, children):
    out = le('II', 1000, 38) + guid(i) + le('II', 13, 18) + tint(-1) * 3 + tmat([3, 1], [0, 0, 0]) + tint(0) + tint(0) + tnum(1.0) + tint(1) + tint(2)
    out += tint(-1) + tnum(1.4) + ttext('1.4') + tint(1) + tint(0) + ttext(name) + b''.join(tnum(x) for x in place)
    # (Its script, its own properties - name, type, value -, three more.)
    second = [ttext(script), tint(1), ttext('w'), tint(2), ttext('1e-07'), tint(1), tint(1), tint(1)]
    out += le('I', len(second)) + b''.join(second)
    out += blob({'acf_custom': '0.5', 'expanded': 0, 'gridAttributeName': '', 'materialuuid': none, 'runsetuprequired': 0, 'use_relative_coordinates': 1})
    return out + le('I', len(children)) + b''.join(children)
m_sio2, m_sin, m_si, none = '{33333333-0000-4000-8000-000000000001}', '{33333333-0000-4000-8000-000000000002}', '{33333333-0000-4000-8000-000000000003}', '{00000000-0000-0000-0000-000000000000}'
m_ln, m_warm = '{33333333-0000-4000-8000-000000000004}', '{33333333-0000-4000-8000-000000000005}'
def diag(a, b, c): return ('matrix', 3, 3, [a, 0, 0, 0, b, 0, 0, 0, c])
c = 299792458.0
f0, f1 = c / 1.6e-6, c / 1.5e-6
materials = [
    # SiO2 sampled at two frequencies about 1550 nm: ε 2.10 and 2.08 (n at 1550 nm between).
    {'name': 'SiO2 (Glass) - Palik', 'type': 7, 'materialuuid': m_sio2, 'priority': 2,
     'frequency': ('matrix', 2, 1, [f0, f1]), 'permittivity': ('complex', [2.10, 2.08], [0.0, 0.0])},
    {'name': 'Si3N4 (Silicon Nitride) - Phillip', 'type': 0, 'materialuuid': m_sin, 'priority': 2,
     'permittivity': ('matrix', 3, 3, [4.0, 0, 0, 0, 4.0, 0, 0, 0, 4.0])},
    {'name': 'Si (Silicon) - Palik', 'type': 7, 'materialuuid': m_si, 'priority': 2,
     'frequency': ('matrix', 2, 1, [f0, f1]), 'permittivity': ('complex', [12.0, 12.2], [0.01, 0.02])},
    # Zelmon's MgO:LN (λ in µm, its poles in µm²), its c axis along z.
    {'name': 'LN_Sellmeier_MgO_Zel', 'type': 6, 'materialuuid': m_ln, 'priority': 2, 'anisotropy': 1,
     'coefficients0': diag(1, 1, 1), 'coefficients1': diag(2.2454, 2.2454, 2.4272), 'coefficients2': diag(0.01242, 0.01242, 0.01478),
     'coefficients3': diag(1.3005, 1.3005, 1.4617), 'coefficients4': diag(0.05313, 0.05313, 0.05612),
     'coefficients5': diag(6.8972, 6.8972, 9.6536), 'coefficients6': diag(331.33, 331.33, 371.216)},
    # Temperature dependent: the silicon's index, dn/dT from 300 K, Soref's carriers' terms.
    {'name': 'silicon_thermal', 'type': 13, 'materialuuid': m_warm, 'priority': 2, 'basematerial': m_si, 'includeTemperatureEffects': 1,
     'dnDt': diag(1.8e-4, 1.8e-4, 1.8e-4), 'dkDt': diag(0, 0, 0), 'tref': diag(300, 300, 300),
     'dnAn': diag(-8.8e-22, -8.8e-22, -8.8e-22), 'dnAp': diag(-8.5e-18, -8.5e-18, -8.5e-18), 'daAn': diag(8.5e-18, 8.5e-18, 8.5e-18), 'daAp': diag(6e-18, 6e-18, 6e-18)},
]
body = b'LUMERICAL file version 1.1\n' + bytes(64)
body += b'Lumerical material data file version 2.0\0' + le('I', len(materials)) + b''.join(blob(m) for m in materials)
fde = solver(10, {'name': '::model::FDE', 'type': 1, 'MODEType': 2, 'GUIx': 0.0, 'GUIy': 1e-7, 'GUIwidth': 4e-6, 'GUIheight': 3e-6, 'dx': 2e-8, 'simulationTemperature': 310.0,
                  'dy': 2e-8, 'refractiveIndex': 1.0, 'modeFrequency': c / 1.55e-6, 'numberOfModes': 6, 'useMaxIndex': 0, 'mRefIndex': '1.9',
                  'stopFrequency': c / 1.6e-6, 'numberOfPoints': 5, 'pmlMODE': ('ints', [1, 1, 0, 0, 0, 0]), 'xMinBC': 2, 'xMaxBC': 2,
                  'yMinBC': 1, 'yMaxBC': 1},
             [solver(11, {'name': '::model::FDE::data', 'type': 14}, []), rect(12, 'rectangle', 1.0, (-3e-6, -3e-6, 3e-6, 3e-6, 0, 0), none, order=2147483646)])
tree = solver(1, {'name': '::model', 'type': 8, 'x': 0.0, 'y': 0.0, 'z': 0.0, 'setupscript': 'a = 1;'}, [
    rect(2, 'substrate', 1.4, (-4e-6, -4e-6, 0.0, 4e-6, -5e-7, 5e-7), m_sio2, order=5),
    rect(3, 'clad', 1.45, (0.0, -4e-6, 4e-6, 4e-6, -5e-7, 5e-7), none),
    rect(4, 'core', 2.0, (0.0, -6e-7, 4e-7, 6e-7, -5e-7, 5e-7), m_sin, order=1),   # (its own order 1: drawn last but for the pad and the warm one)
    rect(5, 'strip', 3.5, (4e-7, -1e-7, 6e-7, 1e-7, -5e-7, 5e-7), m_si),
    # (A pyramid along z, its spans 0.4 by 0.3 µm at its foot, 0.2 by 0.1 at its
    # top: at z = 0 halfway, 0.3 by 0.2 µm about (1.2, 0.5) µm.)
    structure(6, 'pedestal', '1.6', (1.6, 1.2e-6, 5e-7, 4e-7, 3e-7, 2e-7, 1e-7, -5e-7, 5e-7), none, shape=9),
    structure(7, 'other', 1.0, (1.0, 0, 0, 1e-7, 1e-7, -5e-7, 5e-7), none, shape=7),
    # (A structure group at (-1.4, 0.6) µm: a ridge, a pad of the LN under it, its own order 1.)
    group(8, 'LNOI', (-1.4e-6, 6e-7, 0.0), 'deleteall; addpoly; set("vertices", vtx);', [
        polygon('ridge', '2.2;2.1;2.2', [(-3e-7, -1e-7), (3e-7, -1e-7), (1.5e-7, 1e-7), (-1.5e-7, 1e-7)], -5e-7, 5e-7, (0.0, 0.0)),
        rect(0, 'pad', 1.0, (-4e-7, -2e-7, -2e-7, 2e-7, -5e-7, 5e-7), m_ln, order=1, inner=True)]),
    rect(9, 'warm', 3.5, (-6e-7, 1.0e-6, -4e-7, 1.4e-6, -5e-7, 5e-7), m_warm, order=1),
    fde,
    solver(20, {'name': 'mesh', 'x': 0.0, 'y': 2e-7, 'dx': 1e-8, 'dy': 1e-8}, []),
    solver(21, {'name': '::model::temperature', 'type': 3, 'x': 0.0, 'y': 0.0, 'z': 0.0}, [])])
body += tree
with open('lumerical_mode.lms', 'wb') as f:
    f.write(body)

# --- DEVICE 7.17 (CHARGE, HEAT) ------------------------------------------
def models(rec, groups):
    # (Each: its group's name -> (its Name, active model, carrier, models'
    # parameters, more attributes); a plain group: (None, enabled, groups).)
    for link, spec in groups.items():
        g = rec.create_group(link)
        if spec[0] is None:
            g.attrs['enabled'] = np.int32(spec[1])
            models(g, spec[2])
            continue
        name, active, carrier, alternatives, extra = spec
        g.attrs['Active Model'] = np.bytes_(active)
        g.attrs['Name'] = np.bytes_(name)
        g.attrs['Enabled'] = np.int32(1)
        if carrier: g.attrs['Carrier Type'] = np.int32(carrier)
        for k, v in extra.items(): g.attrs[k] = v
        for alt, params in alternatives.items():
            a = g.create_group(alt)
            for k, v in params.items(): a.attrs[k] = np.float64(v)
def material_object(parent, i, uid, alias, records):
    # (Its records: (kind, name, attributes, models) each.)
    g = parent.create_group('Children/%d' % i)
    g.attrs['material uuid'] = np.bytes_(uid)
    props(g, {'name': alias, 'type': 'Material Object'})
    for j, (kind, name, attrs, ms) in enumerate(records):
        r = g.create_group('Children/%d' % (j + 1))
        props(r, {'name': name, 'type': kind})
        rec = r.create_group('Custom/1/{44444444-0000-4000-8000-%012d}' % (10 * i + j))
        rec.attrs['name'] = np.bytes_(name)
        for k, v in attrs.items(): rec.attrs[k] = np.float64(v)
        models(rec, ms)
def data_new(name, values, dims):
    # (DEVICE 7.17's "Data": its name, 0xFFFFFFFF, its dimensions (64-bit), 1 real, the numbers.)
    n = name.encode()
    return le('I', len(n)) + n + le('I', 0xFFFFFFFF) + le('Q', len(dims)) + b''.join(le('Q', d) for d in dims) + b'\x01' + b''.join(le('d', x) for x in values)
def data_old(name, values, dims):
    # (An older one's: its name, then a Properties matrix's layout.)
    n = name.encode()
    return le('I', len(n)) + n + le('II', len(values), len(dims)) + b''.join(le('I', d) for d in dims) + le('II', 1, 1) + b''.join(le('d', x) for x in values)
def constant(name, carrier, A0): return (name, 'Constant', carrier, {'Constant': {'A0': A0}}, {})
uid = {k: '{55555555-0000-4000-8000-%012d}' % i for i, k in enumerate(['Si', 'Ge', 'SiO2', 'Al', 'n-gate', 'Air'], 1)}
si_models = {
    'Bandgap': ('Bandgap', 'Varshni', 0, {'Varshni': {'y0': 1.169, 'alpha': 4.73e-4, 'beta': 636}, 'Constant': {'A0': 1.12}}, {}),
    'Effective Mass (n)': constant('Effective Mass (n)', 110, 1.09), 'Effective Mass (p)': constant('Effective Mass (p)', 112, 1.15),
    'Lattice Mobility (n)': ('Lattice Mobility (n)', 'Universal Temperature', 110, {'Universal Temperature': {'A': 1471, 'eta': -2.42}}, {}),
    'Lattice Mobility (p)': ('Lattice Mobility (p)', 'Universal Temperature', 112, {'Universal Temperature': {'A': 470.5, 'eta': -2.2}}, {}),
    'Impurity Scattering (n)': ('Impurity Scattering (n)', 'Masetti', 110, {'Masetti': {'muMin(300)': 52.2, 'mu2(300)': 43.4, 'Cr(300)': 9.68e16,
                                'Cs(300)': 3.43e20, 'alpha(300)': 0.68, 'beta(300)': 2.0}}, {}),
    'Impurity Scattering (p)': ('Impurity Scattering (p)', 'Masetti', 112, {'Masetti': {'muMin(300)': 44.9, 'mu2(300)': 29.0, 'Cr(300)': 2.23e17,
                                'Cs(300)': 6.1e20, 'alpha(300)': 0.719, 'beta(300)': 2.0}}, {}),
    'High Field Mobility (n)': ('High Field Mobility (n)', 'Monotonic', 110, {'Monotonic': {'beta': 2.0}}, {'Driving Field': np.int32(1)}),
    'High Field Mobility (p)': ('High Field Mobility (p)', 'Monotonic', 112, {'Monotonic': {'beta': 1.0}}, {'Driving Field': np.int32(1)}),
    'Mobility Velocity Saturation (n)': constant('Mobility Velocity Saturation (n)', 110, 1.07e7),
    'Mobility Velocity Saturation (p)': constant('Mobility Velocity Saturation (p)', 112, 8.37e6),
    'Bandgap Narrowing': ('Bandgap Narrowing', 'Slotboom', 110, {'Slotboom': {'V1': 6.92e-3, 'N0': 1.3e17, 'C': 0.5}}, {}),
    'Recombination': (None, 1, {
        'SRH Carrier Lifetime (n)': constant('SRH Carrier Lifetime (n)', 110, 3e-6), 'SRH Carrier Lifetime (p)': constant('SRH Carrier Lifetime (p)', 112, 1e-6),
        # (Its doping dependence: a group of the same name, its models Fossum's.)
        'SRH Doping (n)': ('SRH Carrier Lifetime (n)', 'Fossum', 110, {'Fossum': {'N0': 7.1e15, 'alpha': 1, 'beta': 0, 'gamma': 1, 'sigma': 1}, 'None': {}}, {}),
        'SRH Doping (p)': ('SRH Carrier Lifetime (p)', 'Fossum', 112, {'Fossum': {'N0': 5e15, 'alpha': 1, 'beta': 0, 'gamma': 1, 'sigma': 1}, 'None': {}}, {}),
        'SRH Field (n)': ('SRH Carrier Lifetime Field (n)', 'Hurkx', 110, {'Hurkx': {'mt': 0.25}, 'None': {}}, {}),
        'SRH Field (p)': ('SRH Carrier Lifetime Field (p)', 'Hurkx', 112, {'Hurkx': {'mt': 0.3}, 'None': {}}, {}),
        'Single-Level Trap': constant('Single-Level Trap', 110, 0.1),
        'Auger Recombination (n)': constant('Auger Recombination (n)', 110, 2.8e-31), 'Auger Recombination (p)': constant('Auger Recombination (p)', 112, 9.9e-32)}),
    # (Off: its group not enabled.)
    'Optional': (None, 0, {'Optical Carrier Lifetime': constant('Optical Carrier Lifetime', 110, 1.1e-14)}),
    'Impact Ionization Coefficient (n)': ('Impact Ionization Coefficient (n)', 'Selberherr', 110, {'Selberherr': {'alphaInf(300)': 7.03e5, 'Ecrit(300)': 1.231e6, 'beta': 1}}, {}),
    'Impact Ionization Coefficient (p)': ('Impact Ionization Coefficient (p)', 'Selberherr', 112, {'Selberherr': {'alphaInf(300)': 1.582e6, 'Ecrit(300)': 2.036e6, 'beta': 1}}, {}),
    'Band To Band Tunneling': ('Band To Band Tunneling', 'Hurkx', 0, {'Hurkx': {'A': 4e14, 'B': 1.9e7, 'eta': 2.5}}, {}),
}
ge_models = {
    'Bandgap': constant('Bandgap', 0, 0.66), 'Effective Mass (n)': constant('Effective Mass (n)', 110, 0.56), 'Effective Mass (p)': constant('Effective Mass (p)', 112, 0.29),
    'Lattice Mobility (n)': constant('Lattice Mobility (n)', 110, 3900), 'Lattice Mobility (p)': constant('Lattice Mobility (p)', 112, 1900),
    'SRH Carrier Lifetime (n)': constant('SRH Carrier Lifetime (n)', 110, 1e-9), 'SRH Carrier Lifetime (p)': constant('SRH Carrier Lifetime (p)', 112, 1e-9),
}
def thermal(k, rho, cp):
    return {'Thermal Conductivity': k, 'Mass Density': constant('Mass Density', 0, rho), 'Specific Heat': constant('Specific Heat', 0, cp)}
with h5py.File('lumerical_device.ldev', 'w', libver='latest') as f:
    f.create_group('FileVersionInfo/DEVICE').attrs['version'] = np.bytes_('7.17')
    root = f.create_group('SimObjectRoot')
    props(root, {'name': 'model', 'type': ''})
    mats = root.create_group('Children/1')
    props(mats, {'name': 'materials', 'type': 'Material Group'})
    material_object(mats, 1, uid['Si'], 'Si', [('Semiconductor', 'Si (Silicon)', {'workfunction': 4.2, 'epsilonr': 11.7}, si_models)])
    material_object(mats, 2, uid['Ge'], 'Ge', [('Semiconductor', 'Ge (Germanium)', {'workfunction': 4.6, 'epsilonr': 16.2}, ge_models)])
    material_object(mats, 3, uid['SiO2'], 'SiO2', [('Insulator', 'SiO2 (Glass) - Sze', {'epsilonr': 3.9}, {})])
    material_object(mats, 4, uid['Al'], 'Al', [('Conductor', 'Al (Aluminium) - CRC', {'workfunction': 4.28}, {})])
    material_object(mats, 5, uid['n-gate'], 'n-gate', [('Conductor', 'n-silicon gate', {'workfunction': 3.64}, {})])
    geo = root.create_group('Children/2')
    props(geo, {'name': 'geometry', 'type': 'Geometry Group'})
    def box(i, name, material, x0, x1, z0, z1, order):
        props(geo.create_group('Children/%d' % i), {'name': name, 'type': 'Rectangle', 'material': uid[material], 'mesh order': float(order),
                                                    'x min': x0, 'x max': x1, 'y min': -1e-6, 'y max': 1e-6, 'z min': z0, 'z max': z1,
                                                    'first axis': 0, 'rotation 1': 0.0})
    box(1, 'bottom', 'Al', -2e-6, 2e-6, -1.05e-6, -1e-6, 2)
    box(2, 'body', 'Si', -2e-6, 2e-6, -1e-6, 0.0, 3)
    box(3, 'oxide', 'SiO2', -1e-6, 5e-7, 0.0, 1e-8, 2)
    box(4, 'gate', 'n-gate', -1e-6, 5e-7, 1e-8, 6e-8, 2)
    # A pyramid of germanium on the silicon: cut by y = 0, a trapezoid
    # 1 µm wide at its foot, 0.6 µm at its top 0.3 µm up.
    props(geo.create_group('Children/5'), {'name': 'ge', 'type': 'Pyramid', 'material': uid['Ge'], 'mesh order': 2.0, 'x': 1.25e-6, 'y': 0.0,
                                           'x span bottom': 1e-6, 'x span top': 6e-7, 'y span bottom': 4e-6, 'y span top': 4e-6, 'z min': 0.0, 'z max': 3e-7,
                                           'first axis': 0, 'rotation 1': 0.0})
    props(root.create_group('Children/3'), {'name': 'CHARGE simulation region', 'type': 'Simulation Region', 'dimension': 1,
                                            'x min': -2e-6, 'x max': 2e-6, 'y min': 0.0, 'y max': 0.0, 'z min': -1.05e-6, 'z max': 3e-7})
    solver = f.create_group('SemiconductorSimObject')
    props(solver, {'name': 'CHARGE', 'type': 'Solver', 'norm length': 1e-6, 'simulation temperature': 300.0, 'fermi statistics': 0,
                   'simulation region': 'CHARGE simulation region'}, prefix=bytes(range(16)))
    def child(i, entries, parent=solver):
        g = parent.create_group('Children/%d' % i)
        props(g, entries)
        return g
    child(1, {'name': 'pwell', 'type': 'Constant Doping Region', 'concentration': 1e17, 'dopant type': 1.0, 'volume type': 2, 'volume solid': '',
              'x min': -2e-6, 'x max': 2e-6, 'y min': -1e-6, 'y max': 1e-6, 'z min': -1.05e-6, 'z max': 0.0})
    # (Its box as wide as its junction: a profile without a plateau.)
    child(2, {'name': 'ge p++', 'type': 'Diffusion Doping', 'concentration': 1e19, 'dopant type': 1.0, 'junction width': 2e-8,
              'ref concentration': 1e15, 'source face': 5, 'distribution function': 1, 'volume type': 2, 'volume solid': '',
              'x min': 9.5e-7, 'x max': 1.55e-6, 'y min': -1e-6, 'y max': 1e-6, 'z min': 2.8e-7, 'z max': 3e-7})
    bcs = child(3, {'name': 'boundary conditions', 'type': 'Boundary Condition Group'})
    child(1, {'name': 'gate', 'type': 'Electrical Boundary Condition', 'solid': 'gate', 'surface type': 3, 'sweep type': 1, 'voltage': 0.0,
              'range start': -1.0, 'range stop': 1.0, 'range num points': 5, 'force ohmic': 0, 'bc mode': 0}, bcs)
    child(2, {'name': 'substrate', 'type': 'Electrical Boundary Condition', 'solid': 'bottom', 'surface type': 3, 'sweep type': 0, 'voltage': 0.0,
              'force ohmic': 1, 'bc mode': 0}, bcs)
    child(3, {'name': 'interface', 'type': 'Surface Recombination', 'surface type': 6, 'material 1': 'Si', 'material 2': 'SiO2',
              'electron velocity': 100.0, 'hole velocity': 200.0, 'trap offset': 0.05, 'enabled': 1}, bcs)
    child(4, {'name': 'at metal', 'type': 'Surface Recombination', 'surface type': 6, 'material 1': 'Si', 'material 2': 'Al',
              'electron velocity': 1e5, 'hole velocity': 1e5, 'trap offset': 0.0, 'enabled': 1}, bcs)
    # An imported generation (saved dark: not enabled), its G on x by y by
    # z (x fastest), its scale factor 2 mW; G a 7.17 blob, x an older one's.
    gen = child(4, {'name': 'gen', 'type': 'Optical Generation Rate', 'enabled': 0, 'scale factor': 2e-3, 'source power scaling': 1.0,
                    'use relative coordinates': 0, 'x': 0.0, 'y': 0.0, 'z': 0.0})
    xs, ys, zs = [8e-7, 1.25e-6, 1.7e-6], [0.0], [0.0, 3e-7]
    G = [1e27 * (1 + i) * (1 + k) for k in range(2) for j in range(1) for i in range(3)]
    items = gen.create_group('Children/1/Children/1/Children')
    for j, (nm, blob_) in enumerate([('G', data_new('G', G, [3, 1, 2])), ('x', data_old('x', xs, [3, 1])), ('y', data_new('y', ys, [1, 1])),
                                     ('z', data_new('z', zs, [2, 1]))], 1):
        it = items.create_group(str(j))
        it.attrs['Name'] = np.bytes_(nm)
        it.create_dataset('Data', data=np.frombuffer(blob_, dtype=np.int8))
    child(5, {'name': 'cv', 'type': 'Charge Monitor'})

# A HEAT project: a silicon substrate, an oxide over it, a heater layer in
# the oxide the region's width, air above; the substrate's bottom held at
# 300 K, convection where the oxide meets the air.
with h5py.File('lumerical_heat.ldev', 'w', libver='latest') as f:
    f.create_group('FileVersionInfo/DEVICE').attrs['version'] = np.bytes_('7.17')
    root = f.create_group('SimObjectRoot')
    props(root, {'name': 'model', 'type': ''})
    mats = root.create_group('Children/1')
    props(mats, {'name': 'materials', 'type': 'Material Group'})
    material_object(mats, 1, uid['Si'], 'Si', [('Solid', 'Si (Silicon)', {}, thermal(constant('Thermal Conductivity', 0, 148.0), 2330, 711))])
    material_object(mats, 2, uid['SiO2'], 'SiO2', [('Solid', 'SiO2 (Glass) - Sze', {},
                                                     thermal(('Thermal Conductivity', 'Universal Temperature', 0, {'Universal Temperature': {'A': 1.4, 'eta': 0.1}}, {}), 2200, 730))])
    material_object(mats, 3, uid['Al'], 'Al', [('Solid', 'Al (Aluminium) - CRC', {}, thermal(constant('Thermal Conductivity', 0, 237.0), 2700, 897))])
    material_object(mats, 4, uid['Air'], 'Air', [('Fluid', 'Air', {}, thermal(constant('Thermal Conductivity', 0, 0.026), 1.2, 1005))])
    geo = root.create_group('Children/2')
    props(geo, {'name': 'geometry', 'type': 'Geometry Group'})
    def box(i, name, material, x0, x1, z0, z1, order):
        props(geo.create_group('Children/%d' % i), {'name': name, 'type': 'Rectangle', 'material': uid[material], 'mesh order': float(order),
                                                    'x min': x0, 'x max': x1, 'y min': -1e-6, 'y max': 1e-6, 'z min': z0, 'z max': z1,
                                                    'first axis': 0, 'rotation 1': 0.0})
    box(1, 'substrate', 'Si', -5e-6, 5e-6, -5e-6, 0.0, 3)
    box(2, 'box', 'SiO2', -5e-6, 5e-6, 0.0, 2e-6, 3)
    box(3, 'wire', 'Al', -5e-6, 5e-6, 1.5e-6, 1.7e-6, 1)
    box(4, 'air', 'Air', -5e-6, 5e-6, 2e-6, 4e-6, 3)
    props(root.create_group('Children/3'), {'name': 'HEAT simulation region', 'type': 'Simulation Region', 'dimension': 1,
                                            'x min': -5e-6, 'x max': 5e-6, 'y min': 0.0, 'y max': 0.0, 'z min': -5e-6, 'z max': 4e-6})
    solver = f.create_group('ThermalSimObject')
    props(solver, {'name': 'HEAT', 'type': 'Solver', 'norm length': 1e-6, 'simulation temperature': 300.0}, prefix=bytes(range(16)))
    def child(i, entries, parent=solver):
        g = parent.create_group('Children/%d' % i)
        props(g, entries)
        return g
    bcs = child(1, {'name': 'boundary conditions', 'type': 'Boundary Condition Group'})
    child(1, {'name': 'sink', 'type': 'Temperature Boundary Condition', 'surface type': 4, 'z min': 1, 'temperature': 300.0, 'enabled': 1}, bcs)
    child(2, {'name': 'top', 'type': 'Convection', 'surface type': 6, 'material 1': 'SiO2', 'material 2': 'Air', 'h convection': 10.0,
              'ambient temperature': 300.0, 'convection model': 0, 'enabled': 1}, bcs)
    child(3, {'name': 'heater', 'type': 'Uniform Power Heat Source', 'volume solid': 'wire', 'total power': 1e-5, 'use solver norm length': 1, 'enabled': 1}, bcs)

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
def structure(i, name, index, box, material, shape=6):
    # (Its box: index, y min, x min, y max, x max, z min, z max.)
    y0, x0, y1, x1, z0, z1 = box
    out = le('II', 1000, 38) + guid(i) + le('III', shape, 32, 7) + le('7d', index, y0, x0, y1, x1, z0, z1) + text('%g' % index) + le('II', 1, 0) + text(name)
    out += b'\0\xff\xff\xff\xff' * 3 + le('IIII', 3, 0, 3, 2) + bytes(40)
    out += blob({'acf_custom': '0.5', 'acf_fixed': 0.5, 'backgroundId': '{00000000-0000-0000-0000-000000000000}', 'materialuuid': material,
                 'use_relative_coordinates': 1})
    return out + le('I', 0)
m_sio2, m_sin, m_si, none = '{33333333-0000-4000-8000-000000000001}', '{33333333-0000-4000-8000-000000000002}', '{33333333-0000-4000-8000-000000000003}', '{00000000-0000-0000-0000-000000000000}'
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
]
body = b'LUMERICAL file version 1.1\n' + bytes(64)
body += b'Lumerical material data file version 2.0\0' + le('I', len(materials)) + b''.join(blob(m) for m in materials)
fde = solver(10, {'name': '::model::FDE', 'type': 1, 'MODEType': 2, 'GUIx': 0.0, 'GUIy': 1e-7, 'GUIwidth': 4e-6, 'GUIheight': 3e-6, 'dx': 2e-8,
                  'dy': 2e-8, 'refractiveIndex': 1.0, 'modeFrequency': c / 1.55e-6, 'numberOfModes': 6, 'useMaxIndex': 0, 'mRefIndex': '1.9',
                  'stopFrequency': c / 1.6e-6, 'numberOfPoints': 5, 'pmlMODE': ('ints', [1, 1, 0, 0, 0, 0]), 'xMinBC': 2, 'xMaxBC': 2,
                  'yMinBC': 1, 'yMaxBC': 1},
             [solver(11, {'name': '::model::FDE::data', 'type': 14}, []), structure(12, 'rectangle', 1.0, (-3e-6, -3e-6, 3e-6, 3e-6, 0, 0), none)])
tree = solver(1, {'name': '::model', 'type': 8, 'x': 0.0, 'y': 0.0, 'z': 0.0}, [
    structure(2, 'substrate', 1.4, (-4e-6, -4e-6, 0.0, 4e-6, -5e-7, 5e-7), m_sio2),
    structure(3, 'clad', 1.45, (0.0, -4e-6, 4e-6, 4e-6, -5e-7, 5e-7), none),
    structure(4, 'core', 2.0, (0.0, -6e-7, 4e-7, 6e-7, -5e-7, 5e-7), m_sin),
    structure(5, 'strip', 3.5, (4e-7, -1e-7, 6e-7, 1e-7, -5e-7, 5e-7), m_si),
    structure(6, 'ring', 1.0, (0, 0, 1e-7, 1e-7, -5e-7, 5e-7), none, shape=9),
    fde,
    solver(20, {'name': 'mesh', 'x': 0.0, 'y': 2e-7, 'dx': 1e-8, 'dy': 1e-8}, [])])
body += tree
with open('lumerical_mode.lms', 'wb') as f:
    f.write(body)

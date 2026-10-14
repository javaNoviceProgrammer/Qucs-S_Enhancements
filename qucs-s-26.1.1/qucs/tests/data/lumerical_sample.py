# Writes lumerical_sample.ldev, lumerical_wurtzite.ldev,
# lumerical_mqw_offsets.ldev, lumerical_mqw_workfunctions.ldev and
# lumerical_sample.ldf: small files laid out as Lumerical's MQW projects and
# data files are (as Lumerical 2025's example package has them), for
# test_fem_quantum's importer and test_data_import's .ldf.
#   python3 lumerical_sample.py [file...]   (h5py 3, NumPy; the files named
#                                           alone, else all)
# HDF5 of the latest version (object headers 2, links and attributes dense
# where there are more than eight - a heap of indirect blocks, a B-tree of
# two levels -, fixed-length strings) - an MQW object's
# "Properties" a key/value blob, its layer table a Qt QDataStream's
# dictionary; each material a record of attributes, its models' groups
# GUIDs named by their "Name".
import struct, sys, uuid
import numpy as np, h5py

only = set(sys.argv[1:])

def le(fmt, *v): return struct.pack('<' + fmt, *v)
def be(fmt, *v): return struct.pack('>' + fmt, *v)

def qstring(s):
    b = s.encode('utf-16-be')
    return be('I', len(b)) + b

def qvariant(v):
    if isinstance(v, dict):
        name = b'Dictionary\0'
        return be('I', 0x400) + b'\0' + be('I', len(name)) + name + qdict(v)
    if isinstance(v, str): return be('I', 10) + b'\0' + qstring(v)
    if isinstance(v, int): return be('I', 2) + b'\0' + be('i', v)
    return be('I', 6) + b'\0' + be('d', float(v))

def qdict(d):
    out = be('I', len(d))
    for k, v in d.items(): out += qstring(k) + qvariant(v)
    return out

def blob(entries):
    out = le('I', len(entries))
    for k, v in entries.items():
        key = k.encode() + b'\0'
        out += le('I', len(key)) + key
        if isinstance(v, dict):
            q = qdict(v); out += le('II', 11, len(q)) + q
        elif isinstance(v, str):
            s = v.encode() + b'\0'; out += le('II', 0, len(s)) + s
        elif isinstance(v, int):
            out += le('III', 2, 1, v)
        elif isinstance(v, list):   # a real matrix, a column
            out += le('IIII', 3, 0, len(v), 2) + le('II', len(v), 1) + le('II', 1, 1) + b''.join(le('d', x) for x in v)
        else:
            out += le('IId', 1, 1, float(v))
    return out

def model(group, active, **params):
    g = group.create_group('{%s}' % str(uuid.uuid4()).upper())
    g.attrs['Name'] = np.bytes_(group.attrs.get('_next', b'') or b'')
    return g

def models(parent, name, active, values):
    g = parent.create_group('{%s}' % str(uuid.uuid4()).upper())
    g.attrs['Name'] = np.bytes_(name)
    g.attrs['Active Model'] = np.bytes_(active)
    g.attrs['Enabled'] = np.int32(1)
    for m, ps in values.items():
        sub = g.create_group(m)
        for k, x in ps.items(): sub.attrs[k] = np.float64(x)
    return g

def material(f, index, name, eg, me, eps, zb=None, wz=None, lattice=5.65e-10, stiffness=None, wf=4.5, mh=None):
    obj = f.create_group('SimObjectRoot/Children/1/Children/%d' % index)
    obj.create_dataset('Properties', data=np.frombuffer(blob({'name_value': name, 'type_value': 'Material'}), dtype=np.int8))
    prop = obj.create_group('Children/1')
    uid = '{%s}' % str(uuid.uuid4())
    prop.attrs['Material Properties UID'] = np.bytes_(uid)
    prop.create_dataset('Properties', data=np.frombuffer(blob({'name_value': name + ' (sample)', 'type_value': 'Semiconductor'}), dtype=np.int8))
    rec = prop.create_group('Custom/1/' + uid)
    for k, v in {'name': np.bytes_(name), 'type': np.bytes_('Electrical Material'), 'epsilonr': np.float64(eps), 'alpha': np.float64(1),
                 'delete protect': np.int32(0), 'mesh order': np.int32(2), 'mEpsrIsAnisotropic': np.int32(0),
                 'mEpsrDiagonal': np.array([eps] * 3), 'last modified': np.int64(1415223023), 'workfunction': np.float64(wf)}.items():
        rec.attrs[k] = v   # (ten: dense)
    # (Sixty more: the attributes' heap past its first block - an indirect
    # block's rows.)
    for i in range(60): rec.attrs['spare %02d' % i] = np.float64(i)
    kp = rec.create_group('KdotP')
    kp.attrs['crystal structure'] = np.int32(1 if wz else 0)
    kp.attrs['enable model'] = np.int32(1)
    z = kp.create_group('zincblende parameters')
    for k, v in (zb or {}).items(): z.attrs[k] = np.float64(v)
    w = kp.create_group('wurtzite parameters')
    for k, v in (wz or {}).items(): w.attrs[k] = np.float64(v)
    mech = kp.create_group('mechanical parameters')
    for crystal in ('zincblende parameters', 'wurtzite parameters'):
        m = mech.create_group(crystal)
        C = np.zeros(36)
        if stiffness and (crystal == 'wurtzite parameters') == bool(wz):
            for i, c in stiffness.items(): C[i] = c
        m.attrs['stiffness tensor'] = C
        for lc in ('lattice cte A', 'lattice cte C'):
            g = m.create_group(lc)
            g.attrs['Active Model'] = np.bytes_('Constant')
            g.attrs['Name'] = np.bytes_('Lattice Constant')
            g.create_group('Constant').attrs['A0'] = np.float64(lattice if lc.endswith('A') else 1.6 * lattice)
            q = g.create_group('Quadratic')
            q.attrs['y0'] = np.float64(lattice); q.attrs['alpha'] = np.float64(0); q.attrs['beta'] = np.float64(0)
    valley = rec.create_group('Valley 0')
    valley.attrs['enabled'] = np.int32(1)
    models(valley, 'Bandgap', 'Quadratic', {'Constant': {'A0': eg[0]}, 'Quadratic': {'y0': eg[0], 'alpha': eg[1], 'beta': eg[2]}})
    models(valley, 'Effective Mass (n)', 'Constant', {'Constant': {'A0': me}, 'Quadratic': {'y0': me, 'alpha': 0, 'beta': 0}})
    models(valley, 'Bandgap Narrowing (n)', 'Constant', {'Constant': {'A0': 0}})
    if mh: models(valley, 'Effective Mass (p)', 'Constant', {'Constant': {'A0': mh}, 'Quadratic': {'y0': mh, 'alpha': 0, 'beta': 0}})

# A result dataset's "Data": its name ("null"), -1, its dimensions, 1 for
# real, the doubles column-major.
def data(values):
    v = np.asarray(values, dtype=float).reshape(-1, 1)
    b = le('I', 5) + b'null\0' + le('i', -1) + le('Q', 2) + le('QQ', v.shape[0], 1) + bytes([1]) + v.tobytes(order='F')
    return np.frombuffer(b, dtype=np.int8)

def project(path, settings, layers, materials, results=None, dummies=True):
    if only and path not in only: return
    with h5py.File(path, 'w', libver='latest') as f:
        f.create_group('FileVersionInfo')
        entries = dict(settings)
        entries['layers_value'] = {'size': len(layers), **{'data%d' % i: {'material': m, 'thickness': t, 'strain': s, 'valence band offset': v}
                                                           for i, (m, t, s, v) in enumerate(layers)}}
        mqw = f.create_group('MqwSimObject')
        mqw.create_dataset('Properties', data=np.frombuffer(blob(entries), dtype=np.int8))
        # Its last run's results: each a group named by its Name, each
        # dataset a group of its Name and a Data blob.
        for k, (name, sets) in enumerate((results or {}).items()):
            g = mqw.create_group('Children/1/Children/%d' % (k + 1))
            g.attrs['Name'] = np.bytes_(name)
            for j, (dname, values) in enumerate(sets.items()):
                d = g.create_group('Children/%d' % (j + 1))
                d.attrs['Name'] = np.bytes_(dname)
                d.create_dataset('Data', data=data(values))
        for i, m in enumerate(materials):
            material(f, i + 1, **m)
        # (A hundred material objects: the group's links dense, their names'
        # B-tree of two levels.)
        for i in range(len(materials), 100 if dummies else 0):
            f.create_group('SimObjectRoot/Children/1/Children/%d' % (i + 1))

common = {'max num eigenvalues_value': 10.0, 'name_value': 'MQW', 'temperature_value': 300.0, 'efield type_value': 0, 'cden type_value': 0, 'freq type_value': 0,
          'mesh dz_value': 2e-10, 'gamma_value': 0.0132, 'neff_value': 3.6, 'enable excitons_value': 0, 'override band offset_value': 1,
          'calculate strain_value': 0, 'max num cb subbands_value': 2.0, 'max num vb subbands_value': 4.0, 'kt num points_value': 11.0,
          'pml coefficients_value': [0.5, -1.0]}
gaas = dict(gamma1=6.98, gamma2=2.06, gamma3=2.93, ep=28.8, ac=-7.17, av=1.16, b=-2.0, **{'delta spin-orbit': 0.341})
ingaas = dict(gamma1=7.6, gamma2=2.4, gamma3=3.2, ep=27, ac=-6.9, av=1.1, b=-1.95, **{'delta spin-orbit': 0.35})
project('lumerical_sample.ldev',
        {**common, 'kdotp order_value': 0, 'efield_value': 5e6, 'cden_value': 1e24, 'freq min_value': 3.3e14, 'freq max_value': 3.9e14,
         'freq num points_value': 31.0, 'brillouin zone ratio_value': 0.2},
        [('GaAs (sample)', 8e-9, 0.0, -0.80), ('InGaAs (sample)', 6e-9, -0.01, -0.75), ('GaAs (sample)', 8e-9, 0.0, -0.80)],
        [dict(name='GaAs (sample)', eg=(1.519, 5.405e-4, 204), me=0.067, eps=12.9, zb=gaas, lattice=5.6533e-10,
              stiffness={0: 1.221e11, 6: 5.66e10, 21: 6.0e10}),
         dict(name='InGaAs (sample)', eg=(1.30, 4.5e-4, 180), me=0.058, eps=13.3, zb=ingaas, lattice=5.71e-10,
              stiffness={0: 1.13e11, 6: 5.45e10, 21: 5.6e10})])
gan = {'A1': -6.56, 'A2': -0.91, 'A3': 5.65, 'A4': -2.83, 'A5': -3.13, 'A6': -4.86, 'D1': -0.89, 'D2': 4.27, 'D3': 5.18, 'D4': -2.59,
       'D5': -0.7, 'D6': -5.1, 'a1': -4.08, 'a2': -4.08, 'delta crystal': 0.021, 'delta spin-orbit': 0.016, 'ep': 14.0,
       'me in-plane': 0.2, 'me out-of-plane': 0.2}
algan = {**gan, 'A2': -0.402, 'delta crystal': -0.0258, 'delta spin-orbit': 0.0174, 'ep': 14.1, 'me in-plane': 0.22, 'me out-of-plane': 0.22}
project('lumerical_wurtzite.ldev',
        {**common, 'kdotp order_value': 1, 'efield_value': 0.0, 'cden_value': 4e24, 'freq min_value': 8.2e14, 'freq max_value': 8.7e14,
         'freq num points_value': 11.0, 'brillouin zone ratio_value': 0.1},
        [('AlGaN (sample)', 10e-9, 0.0, 0.0), ('GaN (sample)', 6e-9, -0.00484133952, 0.108256), ('AlGaN (sample)', 10e-9, 0.0, 0.0)],
        [dict(name='AlGaN (sample)', eg=(3.860864, 9.456e-4, 852.4), me=0.22, eps=9.46, wz=algan, lattice=3.1736e-10,
              stiffness={0: 3.912e10, 6: 1.434e10, 12: 1.064e10, 14: 3.93e10, 21: 1.072e10}),
         dict(name='GaN (sample)', eg=(3.5, 7.32e-4, 700), me=0.2, eps=9.7, wz=gan, lattice=3.189e-10,
              stiffness={0: 3.9e10, 6: 1.45e10, 12: 1.06e10, 14: 3.98e10, 21: 1.05e10})])

# Lumerical's own band offsets ("override band offset" 0): one project with
# the band diagram its last run stored (the valence band 0.1095 eV up in the
# well, a field of 30 kV/cm across a stack padded 15 nm each side - its
# photons in eV, "freq type" 2; its excitons on, carriers given; its
# subbands 2 and 2, its cutoffs 0.1 with hard walls), one without (its
# materials' work functions and hole masses; its photons as wavelengths,
# "freq type" 1; PMLs, "boundary condition type" 1).
mod = {**common, 'kdotp order_value': 0, 'efield_value': 3e6, 'freq type_value': 2, 'freq min_value': 1.2, 'freq max_value': 1.5,
       'freq num points_value': 31.0, 'brillouin zone ratio_value': 0.2, 'override band offset_value': 0, 'enable excitons_value': 1,
       'cden_value': 2e24, 'max num cb subbands_value': 2.0, 'max num vb subbands_value': 2.0, 'cutoff cb_value': 0.1, 'cutoff vb_value': 0.1,
       'boundary condition type_value': 0, 'top layer extra thickness_value': 15e-9, 'bottom layer extra thickness_value': 15e-9}
stack = [('GaAs (sample)', 8e-9, 0.0, -0.80), ('InGaAs (sample)', 6e-9, -0.01, -0.75), ('GaAs (sample)', 8e-9, 0.0, -0.80)]
z = np.arange(0.1e-9, 52e-9, 0.2e-9)
inwell = (z > 23e-9) & (z < 29e-9)
ev = np.where(inwell, 0.1095, 0.0) - 3e6 * z
ec = ev + np.where(inwell, 1.25, 1.424)
project('lumerical_mqw_offsets.ldev', mod, stack,
        [dict(name='GaAs (sample)', eg=(1.519, 5.405e-4, 204), me=0.067, eps=12.9, zb=gaas, lattice=5.6533e-10, wf=4.83),
         dict(name='InGaAs (sample)', eg=(1.30, 4.5e-4, 180), me=0.058, eps=13.3, zb=ingaas, lattice=5.71e-10, wf=4.75)],
        results={'banddiagram': {'z': z, 'Ec': ec, 'Ev': ev}}, dummies=False)
project('lumerical_mqw_workfunctions.ldev',
        {**mod, 'freq type_value': 1, 'freq min_value': 0.85e-6, 'freq max_value': 1.0e-6, 'enable excitons_value': 0,
         'boundary condition type_value': 1, 'top layer extra thickness_value': 0.0, 'bottom layer extra thickness_value': 0.0}, stack,
        [dict(name='GaAs (sample)', eg=(1.519, 5.405e-4, 204), me=0.067, eps=12.9, zb=gaas, lattice=5.6533e-10, wf=4.83, mh=0.5),
         dict(name='InGaAs (sample)', eg=(1.30, 4.5e-4, 180), me=0.058, eps=13.3, zb=ingaas, lattice=5.71e-10, wf=4.75, mh=0.45)],
        dummies=False)

# A data file: "curve" two curves of 3 x 2, "table" one matrix of 2 x 3.
def matrix(rows, cols, values):
    return le('IIIII', 1, 0, rows * cols, 2, rows) + le('I', cols) + le('II', 1, 1) + b''.join(le('d', v) for v in values)
if not only or 'lumerical_sample.ldf' in only:
  with open('lumerical_sample.ldf', 'wb') as f:
    f.write(b'Lumerical data file 2, version 1.0' + bytes([0, 3, 4, 8]) + bytes(13) + le('I', 2))
    f.write(le('I', 6) + b'curve\0' + le('I', 2) + matrix(3, 2, [0.1, 0.2, 0.3, 1.0, 2.0, 3.0]) + matrix(3, 2, [0.1, 0.2, 0.3, -1.0, -2.0, -3.0]))
    f.write(le('I', 6) + b'table\0' + le('I', 1) + matrix(2, 3, [1.0, 2.0, 10.0, 20.0, 100.0, 200.0]))
    f.write(le('I', 0))

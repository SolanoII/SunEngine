# Lettura dei corpi esportati dal gioco (.body) e scrittura dei pezzi (.gear). Formato in Engine/Renderer/GearMesh.h.
import struct
import numpy as np

VERT = np.dtype([('pos', '<f4', 3), ('nrm', '<f4', 3), ('uv', '<f4', 2), ('bone', '<i2', 4), ('w', '<f4', 4)])


def _name(f):
    (n,) = struct.unpack('<H', f.read(2))
    return f.read(n).decode('utf-8', 'replace')


def read_body(path):
    """Corpo del gioco: dict con names (indice = id dell'osso nel gioco), offsets (4x4, righe), verts (array VERT),
    idx (triangoli Nx3), aux (bool per vertice: parti aggiunte come muso e zanne)"""
    with open(path, 'rb') as f:
        if f.read(8) != b'SBODY001':
            raise ValueError(f'{path}: non e\' un corpo esportato dal gioco')
        (nb,) = struct.unpack('<I', f.read(4))
        names, offs = [], []
        for _ in range(nb):
            names.append(_name(f))
            m = np.frombuffer(f.read(64), '<f4').reshape(4, 4).T   # glm e' per colonne
            offs.append(m.astype(np.float64))
        (nv,) = struct.unpack('<I', f.read(4))
        verts = np.frombuffer(f.read(nv * VERT.itemsize), VERT).copy()
        (ni,) = struct.unpack('<I', f.read(4))
        idx = np.frombuffer(f.read(ni * 4), '<u4').reshape(-1, 3).astype(np.int64)
    return dict(names=names, offsets=offs, verts=verts, idx=idx, aux=verts['uv'][:, 0] > 0.5)


def write_gear(path, bone_names, pos, nrm, uv, bones, weights, tris):
    """Pezzo per il gioco. pos/nrm in unita' e assi del gioco, bones = indici in bone_names (-1 = nessuno)"""
    n = len(pos)
    v = np.zeros(n, VERT)
    v['pos'] = pos; v['nrm'] = nrm; v['uv'] = uv
    v['bone'] = bones.astype('<i2'); v['w'] = weights
    with open(path, 'wb') as f:
        f.write(b'SGEAR001')
        f.write(struct.pack('<I', len(bone_names)))
        for b in bone_names:
            e = b.encode('utf-8')
            f.write(struct.pack('<H', len(e))); f.write(e)
        f.write(struct.pack('<I', n)); f.write(v.tobytes())
        t = np.ascontiguousarray(tris, dtype='<u4').ravel()
        f.write(struct.pack('<I', len(t))); f.write(t.tobytes())

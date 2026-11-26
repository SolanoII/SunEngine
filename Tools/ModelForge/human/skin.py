# Pelle e occhi del corpo umano (usato da build.py).
#
# Pelle: con Cycles si cuociono nella mappa UV di MakeHuman la posizione e la normale di ogni texel (posa T, metri) e
# l'occlusione ambientale; poi tutto il resto si calcola qui con numpy, texel per texel, con maschere nette:
#   - capelli rasati (attaccatura sulla fronte, basette, nuca), sopracciglia a peli, barba corta, labbra, rossore di
#     guance, naso, orecchie e nocche, occhiaie, rughe della fronte, pori (rilievo e lucido), mutande scure
#   - colore = mappa di DETTAGLIO attorno a 1 (il gioco la moltiplica per la carnagione scelta nel garage)
#   - ORM: R occlusione cotta, G ruvidita' (zona T lucida, labbra umide, capelli opachi), B metallo 0
#   - normali tangenti dal rilievo (pori, rughe, peli corti)
# Occhi: sclera con capillari, iride a raggi, pupilla, cornea lucida.
import math, os
import numpy as np
import bpy
from scipy import ndimage

LM = {}
SC = None
FAST = False


def _activate(ob):
    bpy.context.view_layer.update()
    for o in bpy.context.view_layer.objects:
        if o: o.select_set(False)
    ob.select_set(True); bpy.context.view_layer.objects.active = ob


def _bake_emit(ob, mat, size, socket_fn, float_buf=True, bake_type='EMIT', samples=1):
    nt = mat.node_tree
    out = nt.nodes['Material Output']
    em = nt.nodes.new('ShaderNodeEmission')
    socket_fn(nt, em)
    keep = out.inputs['Surface'].links[0].from_socket if out.inputs['Surface'].links else None
    nt.links.new(em.outputs[0], out.inputs['Surface'])
    im = bpy.data.images.new('tmp_bake', size, size, alpha=False, float_buffer=float_buf)
    im.colorspace_settings.name = 'Non-Color'
    tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
    for n in nt.nodes: n.select = False
    tn.select = True; nt.nodes.active = tn
    SC.render.engine = 'CYCLES'; SC.cycles.device = 'CPU'; SC.cycles.samples = samples
    SC.render.bake.margin = 6
    _activate(ob)
    bpy.ops.object.bake(type=bake_type)
    a = np.empty(size * size * 4, np.float32); im.pixels.foreach_get(a); a = a.reshape(size, size, 4)
    nt.nodes.remove(tn); nt.nodes.remove(em)
    if keep: nt.links.new(keep, out.inputs['Surface'])
    bpy.data.images.remove(im)
    return a


def _save_image(name, arr, noncolor):
    h, w = arr.shape[:2]
    im = bpy.data.images.new(name, w, h, alpha=False)
    if noncolor: im.colorspace_settings.name = 'Non-Color'
    rgba = np.ones((h, w, 4), np.float32); rgba[..., :3] = np.clip(arr, 0, 1)
    im.pixels.foreach_set(rgba.ravel())
    im.pack()
    return im


# ------------------------------------------------------------------ rumore
def _hash3(ix, iy, iz, seed):
    h = (ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF).astype(np.float32) / 16777215.0


def noise3(P, freq, seed=0):
    """rumore a valori 3D, morbido (0..1); a blocchi per non riempire la memoria"""
    if len(P) > 600000:
        return np.concatenate([noise3(P[i:i + 600000], freq, seed) for i in range(0, len(P), 600000)])
    q = P * freq
    i = np.floor(q).astype(np.int64); f = (q - i).astype(np.float32)
    f = f * f * (3 - 2 * f)
    out = 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                out = out + w * _hash3(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz, seed)
    return out


def fbm3(P, freq, octaves=4, seed=0):
    v, a, tot = 0.0, 0.5, 0.0
    for o in range(octaves):
        v = v + a * noise3(P, freq * (2.03 ** o), seed + o * 17); tot += a; a *= 0.5
    return v / tot


def ss(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# ------------------------------------------------------------------ pelle
def apply_skin(body, size):
    m = bpy.data.materials.new('Skin'); m.use_nodes = True
    body.data.materials.clear(); body.data.materials.append(m)
    bs = m.node_tree.nodes['Principled BSDF']
    if FAST:
        bs.inputs['Base Color'].default_value = (0.8, 0.62, 0.5, 1); bs.inputs['Roughness'].default_value = 0.5
        return
    S = size

    def pos(nt, em):
        g = nt.nodes.new('ShaderNodeNewGeometry')
        nt.links.new(g.outputs['Position'], em.inputs['Color'])
    def nor(nt, em):
        g = nt.nodes.new('ShaderNodeNewGeometry')
        v = nt.nodes.new('ShaderNodeVectorMath'); v.operation = 'MULTIPLY_ADD'
        v.inputs[1].default_value = (0.5, 0.5, 0.5); v.inputs[2].default_value = (0.5, 0.5, 0.5)
        nt.links.new(g.outputs['Normal'], v.inputs[0]); nt.links.new(v.outputs[0], em.inputs['Color'])
    def white(nt, em): em.inputs['Color'].default_value = (1, 1, 1, 1)

    print('pelle: cottura di posizione, normale e occlusione', S)
    # posizione: offset per non perdere i negativi, alfa = texel coperto (sfondo 0)
    def posoff(nt, em):
        g = nt.nodes.new('ShaderNodeNewGeometry')
        v = nt.nodes.new('ShaderNodeVectorMath'); v.operation = 'ADD'; v.inputs[1].default_value = (4, 4, 4)
        nt.links.new(g.outputs['Position'], v.inputs[0]); nt.links.new(v.outputs[0], em.inputs['Color'])
    PA = _bake_emit(body, m, S, posoff)
    valid = PA[..., :3].sum(-1) > 1.0
    P = PA[..., :3] - 4.0
    NA = _bake_emit(body, m, S, nor)
    N = NA[..., :3] * 2 - 1
    AO = _bake_emit(body, m, S, white, float_buf=False, bake_type='AO', samples=16)[..., 0]
    AO = ndimage.median_filter(AO, 3)

    idx = np.nonzero(valid.ravel())[0]
    p = P.reshape(-1, 3)[idx]; n = N.reshape(-1, 3)[idx]
    x, y, z = p[:, 0], p[:, 1], p[:, 2]
    eL, eR, mo, jaw, hd, top, nk = LM['l-eye'], LM['r-eye'], LM['mouth'], LM['jaw'], LM['head'], LM['head-2'], LM['neck']
    eyeZ = (eL[2] + eR[2]) / 2; eyeY = (eL[1] + eR[1]) / 2
    headC = np.array([0.0, hd[1], (hd[2] + top[2]) / 2 - 0.02])
    onHead = (np.linalg.norm(p - headC, axis=1) < 0.17) & (z > nk[2] + 0.015)
    front = y < eyeY + 0.03                 # meta' davanti della testa (la faccia guarda verso -Y)
    theta = np.arctan2(np.abs(x), -(y - hd[1]))   # 0 = davanti, pi = dietro

    # --- capelli rasati: attaccatura che scende dalla fronte alle basette e alla nuca
    hl_front, hl_temple, hl_side, hl_back = eyeZ + 0.072, eyeZ + 0.045, eyeZ - 0.005, mo[2] - 0.005
    hairline = np.interp(theta, [0.0, 0.55, 0.95, 1.25, 1.6, 2.2, math.pi], [hl_front, hl_front - 0.004, hl_temple, hl_side, hl_side + 0.01, hl_back, hl_back])
    ear = ss(0.058, 0.068, np.abs(x)) * ss(eyeZ + 0.045, eyeZ + 0.03, z) * ss(eyeZ - 0.06, eyeZ - 0.045, z) * ss(1.1, 1.3, theta) * ss(2.05, 1.85, theta)
    hairEdge = fbm3(p, 180.0, 2, 3) * 0.008
    hair = ss(-0.004, 0.006, z - hairline + hairEdge) * onHead * (1 - ear)
    stip = noise3(p, 2600.0, 5)
    hair_d = hair * (0.95 + 0.05 * ss(0.3, 0.7, stip)) * (0.85 + 0.15 * ss(0.4, 0.8, fbm3(p, 40.0, 2, 6)))

    # --- sopracciglia: arco sopra ogni occhio, peli piu' fitti verso l'interno
    brow = np.zeros_like(x)
    for e, sg in ((eL, 1.0), (eR, -1.0)):
        u = (x - e[0]) * sg                  # verso l'esterno
        arc = e[2] + 0.017 + 0.004 * (1 - ((u - 0.004) / 0.022) ** 2)
        thick = 0.0042 * (1.15 - 0.5 * np.clip((u + 0.012) / 0.04, 0, 1))
        d = np.abs(z - arc)
        mask = ss(thick, thick * 0.4, d) * ss(-0.017, -0.010, u) * ss(0.034, 0.026, u) * (y < e[1] + 0.012)
        brow = np.maximum(brow, mask)
    hairs = noise3(np.c_[x * 1.0, y, z * 6.0], 700.0, 9)   # peli allungati lungo l'arco
    brow_d = brow * (0.45 + 0.55 * ss(0.3, 0.6, hairs))

    # --- barba corta: dal labbro superiore al collo sotto il mento, davanti e ai lati
    noseZ = eyeZ - 0.042
    beardTop = np.interp(theta, [0.0, 0.5, 1.0, 1.35, 1.6], [noseZ - 0.008, noseZ - 0.012, eyeZ - 0.025, eyeZ - 0.005, eyeZ])
    cheekLine = np.interp(theta, [0.0, 0.6, 1.0, 1.35], [noseZ - 0.008, noseZ - 0.02, eyeZ - 0.03, eyeZ - 0.01])
    beard = ss(0.004, -0.004, z - np.minimum(beardTop, cheekLine)) * ss(jaw[2] - 0.075, jaw[2] - 0.04, z) * (theta < 1.65) * onHead
    lipZ = eyeZ - 0.066                  # riga della bocca (il punto 'mouth' di MakeHuman e' piu' in alto, nella cavita')
    lipsE = ((x - mo[0]) / 0.026) ** 2 + ((z - lipZ) / 0.0095) ** 2
    lips = ss(1.15, 0.7, lipsE) * front * (y < mo[1] - 0.015)
    beard = beard * (1 - ss(0.4, 0.0, lipsE - 1.2))
    beard_d = beard * (0.35 + 0.65 * ss(0.45, 0.7, noise3(p, 1100.0, 13)))

    # --- rossore: guance, naso, orecchie; occhiaie; mani e piedi
    red = np.zeros_like(x)
    for e in (eL, eR):
        red += 0.6 * np.exp(-(((x - e[0] * 1.25) ** 2 + (z - (eyeZ - 0.035)) ** 2) / (2 * 0.016 ** 2))) * front
    nose = np.exp(-((x ** 2 + (z - noseZ) ** 2) / (2 * 0.011 ** 2))) * (y < eyeY - 0.005)
    red += 0.3 * nose + 0.5 * ear * onHead
    sock = np.zeros_like(x)
    for e in (eL, eR):
        sock += np.exp(-(((x - e[0]) ** 2 + ((z - (e[2] - 0.012)) * 1.6) ** 2) / (2 * 0.011 ** 2))) * front
    extremity = ss(0.55, 0.7, np.abs(x)) + ss(0.12, 0.04, z)        # mani (in posa T sono lontane) e piedi
    red += 0.35 * extremity

    # --- mutande scure (il gioco veste sopra, ma in garage senza vesti resta decente)
    hipsZ = LM['hips'][2]
    shorts = ss(hipsZ + 0.11, hipsZ + 0.08, z) * ss(hipsZ - 0.2, hipsZ - 0.17, z - 0.06 * np.abs(x) / 0.2) * (np.abs(x) < 0.25)
    # maglietta aderente sotto l'uniforme (busto e meta' braccio): dove la stoffa del gioco si apre nelle pose
    # (ascelle, spalle) si vede stoffa, non pelle
    collar = LM['neck'][2] - 0.035 - 0.03 * ss(0.0, 0.08, np.abs(x))
    shirt = ss(hipsZ + 0.05, hipsZ + 0.09, z) * ss(collar + 0.006, collar - 0.006, z) * ss(0.42, 0.38, np.abs(x)) * (~onHead)
    shorts = np.maximum(shorts, shirt)

    # --- variazione della pelle
    blot = fbm3(p, 14.0, 4, 21)
    fine = fbm3(p, 90.0, 3, 31)

    # colore di dettaglio (attorno a 1, moltiplicato per la carnagione)
    col = np.ones((len(x), 3), np.float32)
    col *= (0.9 + 0.12 * (blot[:, None] - 0.5) * np.array([1.0, 0.8, 0.7]) + 0.05 * (fine[:, None] - 0.5))
    col *= 1 - np.clip(red[:, None], 0, 1) * np.array([0.0, 0.17, 0.15])                    # piu' rosso
    col *= 1 - np.clip(sock[:, None], 0, 1) * np.array([0.12, 0.16, 0.08])                  # occhiaie
    col = col * (1 - lips[:, None] * np.array([0.05, 0.3, 0.26]))                          # labbra
    col = col * (1 - beard_d[:, None] * np.array([0.32, 0.34, 0.33]))                      # barba (bluastra)
    hairCol = np.array([0.035, 0.03, 0.027])   # fattore: moltiplicato per la carnagione diventa quasi nero
    col = col * (1 - hair_d[:, None]) + hair_d[:, None] * hairCol
    col = col * (1 - brow_d[:, None]) + brow_d[:, None] * hairCol * 0.9
    col = col * (1 - shorts[:, None]) + shorts[:, None] * np.array([0.12, 0.12, 0.11])
    # sporco di trincea: fuliggine e terra su fronte, zigomi, collo e mani
    smudge = ss(0.55, 0.75, fbm3(p, 9.0, 4, 51)) * (0.6 * onHead + 0.4 * extremity + 0.25)
    col *= (1 - 0.22 * smudge)[:, None] * np.array([1.0, 0.97, 0.93])
    ao = AO.reshape(-1)[idx]
    col *= (0.75 + 0.25 * ao)[:, None]                                                     # sporco nelle pieghe

    # ruvidita'
    tzone = np.exp(-((x ** 2) / (2 * 0.02 ** 2))) * ss(noseZ - 0.02, noseZ, z) * ss(top[2] - 0.05, eyeZ + 0.03, z) * front
    rough = 0.55 + 0.08 * (fine - 0.5) - 0.07 * tzone - 0.14 * lips + 0.22 * np.maximum(hair, brow) + 0.15 * beard + 0.3 * shorts
    rough = np.clip(rough, 0.25, 0.95)

    # rilievo (in UV): pori fini ovunque, piu' marcati su naso e guance; rughe della fronte; peli corti
    def scatter(vals, fill=0.0):
        img = np.full(S * S, fill, np.float32); img[idx] = vals; return img.reshape(S, S)
    poreMask = scatter(0.6 + 0.4 * np.clip(red + tzone, 0, 1) - 0.5 * np.maximum(hair, beard), 0.6)
    rng = np.random.default_rng(7)
    pores = ndimage.gaussian_filter(rng.random((S, S), np.float32), 1.1)
    pores = (pores - pores.mean()) / (pores.std() + 1e-6)
    pores = -np.clip(pores, -3, 0.6)          # buchini: il rilievo va verso l'interno
    forehead = (ss(eyeZ + 0.03, eyeZ + 0.045, z) * ss(eyeZ + 0.075, eyeZ + 0.06, z) * front * (np.abs(x) < 0.045))
    wr = np.sin((z - eyeZ) * 2 * math.pi / 0.0085 + fbm3(p, 60.0, 2, 41) * 4.0) * forehead
    height = pores * poreMask * 0.35 + scatter(wr * 0.9 + stip * hair * 0.6 + beard_d * 0.4)
    gy, gx = np.gradient(ndimage.gaussian_filter(height, 0.6))
    k = 0.45 * (S / 2048.0)
    nm = np.dstack([-gx * k, -gy * k, np.ones_like(gx)])
    nm /= np.linalg.norm(nm, axis=2, keepdims=True)
    nrm = nm * 0.5 + 0.5

    # immagini (l'origine delle immagini di Blender e' in basso: le righe sono gia' giuste)
    C = np.zeros((S * S, 3), np.float32) + np.array([0.8, 0.75, 0.72]); C[idx] = col
    R = np.zeros((S * S, 3), np.float32); R[:, 0] = 1.0; R[:, 1] = 0.55
    R[idx, 0] = 0.4 + 0.6 * ao; R[idx, 1] = rough; R[:, 2] = 0.0
    C = C.reshape(S, S, 3); R = R.reshape(S, S, 3)
    # i texel fuori dalle isole prendono il colore del bordo (niente aloni con i mipmap)
    inv = ~valid
    if inv.any():
        _, (iy, ix) = ndimage.distance_transform_edt(inv, return_indices=True)
        C = C[iy, ix]; R = R[iy, ix]; nrm = np.where(inv[..., None], np.array([0.5, 0.5, 1.0]), nrm)
    # il colore si salva in sRGB (la mappa e' un fattore: gamma 2.2 per tenere la variazione dove serve)
    imC = _save_image('Skin_BaseColor', np.power(np.clip(C, 0, 1), 1 / 2.2), False)
    imR = _save_image('Skin_ORM', R, True)
    imN = _save_image('Skin_Normal', nrm, True)
    nt = m.node_tree; L = nt.links.new
    def tex(im):
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = im; return t
    L(tex(imC).outputs['Color'], bs.inputs['Base Color'])
    sp = nt.nodes.new('ShaderNodeSeparateColor'); L(tex(imR).outputs['Color'], sp.inputs['Color'])
    L(sp.outputs['Green'], bs.inputs['Roughness']); bs.inputs['Metallic'].default_value = 0.0
    nmn = nt.nodes.new('ShaderNodeNormalMap'); L(tex(imN).outputs['Color'], nmn.inputs['Color']); L(nmn.outputs['Normal'], bs.inputs['Normal'])
    # occlusione nel glTF: il nodo glTF Material Output
    print('pelle cotta')


# ------------------------------------------------------------------ occhi
def apply_eyes(eyes, size=512):
    """cuoce l'occhio sul primo (polo della sfera = pupilla, UV della sfera) e usa lo stesso materiale per tutti"""
    m = bpy.data.materials.new('Eye'); m.use_nodes = True
    nt = m.node_tree; L = nt.links.new
    bs = nt.nodes['Principled BSDF']
    for e in eyes: e.data.materials.clear(); e.data.materials.append(m)
    e = eyes[0]
    if not FAST:
        # colore dalle coordinate oggetto: +Z locale = direzione dello sguardo
        tc = nt.nodes.new('ShaderNodeTexCoord')
        sep = nt.nodes.new('ShaderNodeSeparateXYZ'); L(tc.outputs['Object'], sep.inputs['Vector'])
        vm = nt.nodes.new('ShaderNodeVectorMath'); vm.operation = 'NORMALIZE'; L(tc.outputs['Object'], vm.inputs[0])
        sep2 = nt.nodes.new('ShaderNodeSeparateXYZ'); L(vm.outputs[0], sep2.inputs['Vector'])
        ang = nt.nodes.new('ShaderNodeMath'); ang.operation = 'ARCCOSINE'; L(sep2.outputs['Z'], ang.inputs[0])
        def mr(val, a, b, c, d):
            n = nt.nodes.new('ShaderNodeMapRange'); n.clamp = True
            n.inputs['From Min'].default_value = a; n.inputs['From Max'].default_value = b
            n.inputs['To Min'].default_value = c; n.inputs['To Max'].default_value = d
            L(val, n.inputs['Value']); return n.outputs['Result']
        def mix(fac, a, b):
            n = nt.nodes.new('ShaderNodeMix'); n.data_type = 'RGBA'
            if isinstance(fac, float): n.inputs['Factor'].default_value = fac
            else: L(fac, n.inputs['Factor'])
            for sock, v in ((6, a), (7, b)):
                if isinstance(v, tuple): n.inputs[sock].default_value = (*v, 1)
                else: L(v, n.inputs[sock])
            return n.outputs[2]
        # sclera con capillari verso i bordi
        vein = nt.nodes.new('ShaderNodeTexNoise'); vein.inputs['Scale'].default_value = 40.0; vein.inputs['Detail'].default_value = 6.0
        L(tc.outputs['Object'], vein.inputs['Vector'])
        vmask = mr(vein.outputs['Fac'], 0.55, 0.62, 0.0, 1.0)
        edge = mr(ang.outputs[0], 0.9, 1.6, 0.0, 0.7)
        vm2 = nt.nodes.new('ShaderNodeMath'); vm2.operation = 'MULTIPLY'; L(vmask, vm2.inputs[0]); L(edge, vm2.inputs[1])
        c = mix(vm2.outputs[0], (0.72, 0.68, 0.62), (0.55, 0.18, 0.15))
        # iride: raggi bruni, anello scuro al bordo
        rays = nt.nodes.new('ShaderNodeTexNoise'); rays.inputs['Scale'].default_value = 25.0
        mp = nt.nodes.new('ShaderNodeMapping'); mp.inputs['Scale'].default_value = (60.0, 60.0, 0.3)
        L(vm.outputs[0], mp.inputs['Vector']); L(mp.outputs['Vector'], rays.inputs['Vector'])
        iris = mix(rays.outputs['Fac'], (0.05, 0.025, 0.012), (0.22, 0.12, 0.05))
        ring = mr(ang.outputs[0], 0.38, 0.46, 0.0, 1.0)
        iris = mix(ring, iris, (0.02, 0.012, 0.008))
        inIris = mr(ang.outputs[0], 0.47, 0.45, 0.0, 1.0)
        c = mix(inIris, c, iris)
        pup = mr(ang.outputs[0], 0.17, 0.15, 0.0, 1.0)
        c = mix(pup, c, (0.005, 0.005, 0.005))
        im = bpy.data.images.new('Eye_BaseColor', size, size, alpha=False)
        em = nt.nodes.new('ShaderNodeEmission'); L(c, em.inputs['Color'])
        out = nt.nodes['Material Output']; L(em.outputs[0], out.inputs['Surface'])
        tn = nt.nodes.new('ShaderNodeTexImage'); tn.image = im
        for n_ in nt.nodes: n_.select = False
        tn.select = True; nt.nodes.active = tn
        SC.render.engine = 'CYCLES'; SC.cycles.samples = 1; SC.render.bake.margin = 4
        _activate(e); bpy.ops.object.bake(type='EMIT')
        im.pack()
        for n_ in list(nt.nodes):
            if n_ not in (out, bs): nt.nodes.remove(n_)
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = im
        L(t.outputs['Color'], bs.inputs['Base Color']); L(bs.outputs[0], out.inputs['Surface'])
    else:
        bs.inputs['Base Color'].default_value = (0.7, 0.66, 0.6, 1)
    bs.inputs['Roughness'].default_value = 0.06
    # trasformazioni applicate: il gioco legge i vertici
    for ob in eyes:
        _activate(ob); bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

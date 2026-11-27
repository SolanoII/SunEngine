#!/usr/bin/env python3
"""Icone di SunCollector: fazioni, armi, armature, equipaggiamento.

Stile: disegno a contorno nero spesso, interno bianco/grigio chiaro, sfondo trasparente (come un doodle in bianco e
nero). Ogni icona e' un SVG 256x256 disegnato qui in codice e convertito in PNG con rsvg-convert.

  python3 Tools/Icons/icons.py            -> Sandbox/GameData/Icons/{Factions,Items}/<id>.png (+ sheet.png di prova)
  python3 Tools/Icons/icons.py --svg      -> salva anche gli SVG in Tools/Icons/svg

Ogni oggetto del gioco prende l'icona del suo archetipo (ITEM_ARCH, poi le regole per nome in arch_of). Un oggetto
nuovo senza regola usa l'icona generica del suo tipo: per dargliene una giusta basta aggiungerlo a ITEM_ARCH.
"""
import glob, json, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GD = os.path.join(ROOT, 'Sandbox', 'GameData')
OUT = os.path.join(GD, 'Icons')

K = '#141414'      # contorno
W = '#ffffff'      # pieno
G = '#d4d4d0'      # ombra chiara
G2 = '#a9a9a4'     # ombra scura
SW = 7             # spessore del contorno
SD = 4             # spessore dei dettagli


# ------------------------------------------------------------------------------------------------ primitive
def _st(fill, w):
    return f'fill="{fill}" stroke="{K}" stroke-width="{w}" stroke-linejoin="round" stroke-linecap="round"'

def poly(pts, fill=W, w=SW):
    return f'<polygon points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in pts)}" {_st(fill, w)}/>'

def rect(x, y, w_, h, fill=W, rx=0, w=SW):
    return f'<rect x="{x}" y="{y}" width="{w_}" height="{h}" rx="{rx}" {_st(fill, w)}/>'

def circ(cx, cy, r, fill=W, w=SW):
    return f'<circle cx="{cx}" cy="{cy}" r="{r}" {_st(fill, w)}/>'

def ell(cx, cy, rx, ry, fill=W, w=SW):
    return f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}" {_st(fill, w)}/>'

def line(x1, y1, x2, y2, w=SD):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{K}" stroke-width="{w}" stroke-linecap="round"/>'

def path(d, fill=W, w=SW):
    return f'<path d="{d}" {_st(fill, w)}/>'

def lines(*segs, w=SD):
    return ''.join(line(*s, w=w) for s in segs)

def hatch(x0, y0, x1, y1, n, w=3):
    """tratteggio diagonale dentro un rettangolo (ombra da doodle)"""
    out = []
    for i in range(1, n + 1):
        t = i / (n + 1)
        x = x0 + (x1 - x0) * t
        out.append(line(x, y1, x + (y1 - y0) * 0.5, y0, w=w))
    return ''.join(out)

def group(body, rot=-35, s=1.0, dx=0, dy=0):
    return f'<g transform="translate({128 + dx} {128 + dy}) rotate({rot}) scale({s})">{body}</g>'

def svg(body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">{body}</svg>'


# ------------------------------------------------------------------------------------------------ armi da fuoco
# disegnate in orizzontale (canna a destra, x da -130 a 130), poi ruotate in diagonale
def grip(x, y, ang=18, h=52, wd=26):
    a = math.radians(ang)
    dx = math.sin(a) * h
    return poly([(x, y), (x + wd, y), (x + wd - dx, y + h), (x - dx, y + h)])

def pistol(kind='ballistic'):
    b = ''
    if kind == 'heavy':
        b += grip(-48, 10, 14, 56, 30)
        b += rect(-60, -26, 120, 40, rx=5)                  # carrello grosso
        b += rect(58, -20, 30, 26, rx=3)                    # canna corta e grossa
        b += circ(-18, 26, 14)                              # ponticello
        b += hatch(-50, -22, 40, 8, 5)
        b += lines((-20, -26, -20, 14), (20, -26, 20, 14))
        b += rect(86, -24, 10, 34, fill=G, rx=2)            # bocca
    elif kind == 'laser':
        b += grip(-44, 8, 16, 52, 26)
        b += rect(-58, -24, 110, 32, rx=6)
        b += rect(-30, -40, 54, 16, fill=G, rx=4)           # cella d'energia
        b += lines((-18, -40, -18, -24), (-6, -40, -6, -24), (6, -40, 6, -24))
        b += rect(50, -18, 36, 20, rx=4)
        for i in range(3): b += rect(54 + i * 10, -26, 5, 36, fill=W, rx=1, w=4)   # alette
        b += circ(-10, 24, 12)
    else:
        b += grip(-42, 6, 16, 54, 26)
        b += rect(-56, -24, 116, 30, rx=4)
        b += lines(*[(x, -24, x - 4, -8) for x in (-46, -38, -30)])   # zigrinatura
        b += rect(58, -16, 18, 14, fill=G, rx=2)
        b += circ(-8, 22, 12)
    return group(b, s=1.05, dx=-6, dy=4)

def rifle(kind='ballistic', scope=False, mag='box', length=1.0, bipod=False, heavy=False):
    L = 120 * length
    b = ''
    # calcio
    b += poly([(-L - 10, -14), (-L + 40, -10), (-L + 40, 16), (-L - 4, 30), (-L - 12, 26)])
    b += line(-L - 2, -6, -L - 2, 22)
    # impugnatura
    b += grip(-L + 52, 10, 20, 44, 22)
    # castello
    th = 34 if heavy else 28
    b += rect(-L + 36, -18, 110, th, rx=4)
    # caricatore
    if mag == 'box':
        b += poly([(-L + 92, 10), (-L + 118, 10), (-L + 124, 54), (-L + 98, 56)])
        b += lines((-L + 100, 22, -L + 118, 22), (-L + 102, 34, -L + 120, 34))
    elif mag == 'curve':
        b += path(f'M{-L+92},10 L{-L+118},10 Q{-L+124},40 {-L+136},56 L{-L+110},62 Q{-L+98},40 {-L+92},10 Z')
    elif mag == 'drum':
        b += circ(-L + 106, 36, 26); b += circ(-L + 106, 36, 9, fill=G, w=4)
    elif mag == 'cell':
        b += rect(-L + 88, 10, 38, 22, fill=G, rx=3)
        b += lines((-L + 98, 10, -L + 98, 32), (-L + 108, 10, -L + 108, 32), (-L + 118, 10, -L + 118, 32))
    # canna e guardamano
    bx = -L + 146
    b += rect(bx, -14, 70 * length + 24, 20 if heavy else 16, rx=3)
    if kind == 'laser':
        for i in range(4): b += rect(bx + 10 + i * 16, -22, 8, 32, fill=W, rx=2, w=4)
        b += rect(bx + 70 * length + 16, -18, 16, 24, fill=G, rx=3)
    elif kind == 'heavy_round':
        b += rect(bx + 70 * length + 12, -22, 22, 32, rx=3)
        b += lines((bx + 70 * length + 20, -22, bx + 70 * length + 20, 10), (bx + 70 * length + 27, -22, bx + 70 * length + 27, 10))
    else:
        b += rect(bx, -18, 48, 26, fill=G, rx=3)
        b += lines((bx + 10, -18, bx + 10, 8), (bx + 22, -18, bx + 22, 8), (bx + 34, -18, bx + 34, 8))
        b += rect(bx + 70 * length + 16, -16, 12, 20, rx=2)
    if scope:
        b += rect(-L + 60, -44, 76, 18, rx=8)
        b += rect(-L + 52, -46, 12, 22, fill=G, rx=3); b += rect(-L + 132, -46, 12, 22, fill=G, rx=3)
        b += lines((-L + 80, -26, -L + 80, -18), (-L + 116, -26, -L + 116, -18))
    if bipod:
        b += lines((bx + 40, 4, bx + 20, 44), (bx + 40, 4, bx + 60, 44), w=6)
    b += hatch(-L + 40, -14, -L + 86, 10, 4)
    return group(b, s=0.72 if length > 1.1 else 0.84)

def shotgun(kind='pump'):
    b = ''
    b += poly([(-140, -10), (-90, -8), (-90, 14), (-136, 30), (-144, 26)])
    b += grip(-80, 8, 22, 40, 22)
    b += rect(-94, -16, 70, 28, rx=4)
    b += rect(-26, -14, 156, 12, rx=3)                       # canna
    b += rect(-26, 0, 150, 10, rx=3)                         # tubo serbatoio
    b += rect(30, -4, 50, 20, fill=G, rx=4)                  # pompa
    b += lines((40, -4, 40, 16), (52, -4, 52, 16), (64, -4, 64, 16))
    return group(b, s=0.9)

def blunderbuss():
    b = poly([(-140, -8), (-80, -12), (-80, 16), (-130, 34), (-146, 26)], fill=G)
    b += hatch(-136, -6, -86, 14, 4)
    b += grip(-74, 10, 24, 40, 20)
    b += rect(-86, -16, 46, 26, rx=4)
    b += poly([(-44, -12), (80, -16), (130, -38), (134, 30), (80, 10), (-44, 10)])   # canna a campana
    b += ell(130, -4, 8, 34, fill=G)
    b += lines((0, -13, 0, 10), (40, -15, 40, 10))
    return group(b, s=0.85)

def crude_gun(heavy=False):
    b = poly([(-140, -10), (-96, -12), (-90, 18), (-134, 26)], fill=G)
    b += grip(-84, 10, 26, 42, 22)
    b += poly([(-98, -22), (-20, -26), (-10, 16), (-92, 18)])          # lamiera storta
    b += lines((-80, -16, -78, 10), (-40, -20, -38, 12))
    for x, y in ((-86, -14), (-30, -18), (-84, 10), (-24, 8)): b += circ(x, y, 4, fill=K, w=1)   # bulloni
    b += rect(-12, -14, 120 if not heavy else 140, 16, rx=2)             # tubo
    b += rect(30, -20, 16, 28, fill=G, rx=2); b += rect(70, -20, 12, 28, fill=G, rx=2)   # fascette
    if heavy: b += poly([(-60, 18), (-30, 18), (-26, 58), (-56, 58)]); b += line(-58, 34, -28, 34)
    b += line(-10, -22, 30, -34, w=5)
    return group(b, s=0.88)

def flamer():
    b = poly([(-140, -10), (-92, -10), (-92, 14), (-132, 28)])
    b += grip(-84, 8, 20, 40, 22)
    b += rect(-96, -18, 80, 28, rx=4)
    b += rect(-80, 12, 90, 30, rx=15, fill=G)                 # serbatoio
    b += lines((-60, 12, -60, 42), (-20, 12, -20, 42))
    b += rect(-18, -12, 110, 14, rx=3)
    b += poly([(90, -18), (120, -24), (120, 10), (90, 6)])     # ugello
    b += path('M126,-14 Q146,-26 140,-4 Q156,-6 144,10 Q134,6 128,4 Z', fill=G)   # fiammella
    return group(b, s=0.78)

def plasma():
    b = poly([(-140, -10), (-92, -10), (-92, 14), (-132, 28)])
    b += grip(-84, 8, 20, 40, 22)
    b += rect(-96, -20, 70, 32, rx=6)
    b += ell(-6, -4, 30, 26, fill=G)                           # camera
    b += circ(-6, -4, 12)
    b += rect(22, -12, 96, 16, rx=3)
    for i in range(4): b += ell(36 + i * 20, -4, 7, 18, fill=W, w=4)   # bobine
    b += rect(112, -16, 14, 24, fill=G, rx=3)
    return group(b, s=0.8)

def fusion():
    b = poly([(-140, -10), (-92, -10), (-92, 14), (-132, 28)])
    b += grip(-84, 8, 20, 40, 22)
    b += rect(-96, -22, 90, 34, rx=5)
    b += rect(-60, 12, 40, 18, fill=G, rx=3)
    b += poly([(-8, -22), (110, -34), (124, -22), (10, -6)])   # rebbio sopra
    b += poly([(-8, 12), (110, 24), (124, 12), (10, -2)])      # rebbio sotto
    b += circ(-4, -5, 8, fill=G, w=4)
    b += lines((40, -12, 40, 4), (70, -16, 70, 8))
    return group(b, s=0.8)

def launcher(kind='grenade'):
    b = ''
    if kind == 'grenade':
        b += poly([(-140, -8), (-96, -8), (-96, 16), (-134, 28)])
        b += grip(-86, 10, 20, 40, 22)
        b += rect(-100, -20, 40, 32, rx=4)
        b += ell(-38, -4, 26, 30, fill=G)                     # tamburo
        b += lines((-56, -4, -20, -4), (-38, -32, -38, 24))
        b += rect(-14, -22, 130, 36, rx=6)                    # tubo largo
        b += ell(116, -4, 7, 18, fill=G2)
    else:
        b += rect(-140, -22, 270, 40, rx=8)                    # tubo
        b += grip(-40, 18, 10, 40, 22); b += grip(10, 18, 10, 34, 20)
        b += rect(-20, -40, 50, 18, fill=G, rx=4)              # mirino
        b += path('M130,-22 L160,-2 L130,18 Z', fill=G)        # ogiva del razzo
        b += rect(-150, -28, 14, 52, fill=G, rx=3)
        b += hatch(-120, -18, -50, 14, 5)
    return group(b, s=0.8)

def gravity_cannon():
    b = poly([(-140, -10), (-96, -10), (-96, 16), (-132, 28)])
    b += grip(-88, 10, 20, 40, 22)
    b += rect(-100, -22, 100, 36, rx=6)
    b += rect(-2, -10, 50, 12, rx=3)
    b += ell(76, -4, 18, 44); b += ell(76, -4, 8, 26, fill=G, w=4)       # anello
    b += ell(110, -4, 12, 30, fill=W, w=4)
    return group(b, s=0.85)

def psionic_cannon():
    b = poly([(-140, -10), (-96, -10), (-96, 16), (-132, 28)])
    b += grip(-88, 10, 20, 40, 22)
    b += rect(-100, -22, 100, 36, rx=6)
    b += rect(-2, -12, 40, 16, rx=3)
    b += poly([(40, -4), (78, -40), (120, -4), (78, 32)], fill=W)        # gemma
    b += poly([(56, -4), (78, -24), (100, -4), (78, 16)], fill=G, w=4)
    b += line(78, -40, 78, 32, w=3)
    return group(b, s=0.85)

def turret():
    b = lines((0, 30, -60, 100), (0, 30, 60, 100), (0, 30, 0, 104), w=8)
    b += rect(-60, -10, 130, 40, rx=8)
    b += rect(60, -2, 70, 22, rx=4)
    for i in range(3): b += rect(74 + i * 16, -8, 7, 34, fill=W, rx=2, w=4)
    b += rect(-40, -36, 50, 26, fill=G, rx=4)
    return group(b, rot=-12, s=0.8, dy=-14)


# ------------------------------------------------------------------------------------------------ armi bianche
def handle(x0, x1, h=14):
    b = rect(x0, -h / 2, x1 - x0, h, fill=G, rx=4)
    for x in range(int(x0) + 8, int(x1) - 4, 10): b += line(x, -h / 2, x - 4, h / 2, w=3)
    return b

def knife():
    b = handle(-96, -36, 18)
    b += rect(-40, -20, 10, 40, rx=3)                         # guardia
    b += path('M-30,-11 L70,-11 L104,0 Q80,10 -30,10 Z')
    b += line(-26, -4, 60, -4, w=3)
    return group(b, s=1.0)

def toothsword(two_handed=False):
    L = 170 if two_handed else 140
    b = handle(-L + 10, -L + 70)
    b += rect(-L + 66, -26, 14, 52, rx=4)
    b += path(f'M{-L+80},-16 L{L-10},-16 Q{L+8},-2 {L-10},14 L{-L+80},14 Z')
    # denti della catena
    x = -L + 86
    teeth = []
    while x < L - 14:
        teeth.append(f'L{x+6},-26 L{x+12},-16')
        x += 12
    b += path(f'M{-L+86},-16 ' + ' '.join(teeth), fill=W, w=4)
    b += rect(-L + 84, -8, (L - 10) - (-L + 84) - 6, 10, fill=G, rx=5, w=3)  # motore/catena
    return group(b, s=0.78 if two_handed else 0.88)

def cleaver(heavy=False):
    sc = 1.2 if heavy else 1.0
    b = handle(-110 * sc, -40, 18)
    b += path(f'M-44,-14 L{70*sc},-40 Q{96*sc},-30 {96*sc},{10*sc} L{-44},{26*sc} Z')
    b += circ(60 * sc, -18, 7, fill=W, w=4)
    b += line(-30, 18 * sc, 90 * sc, 4, w=3)
    return group(b, s=0.85 if heavy else 0.95)

def longsword():
    b = handle(-150, -84, 16)
    b += circ(-156, 0, 12)                                     # pomolo
    b += rect(-84, -38, 14, 76, rx=4)                          # elsa
    b += path('M-70,-12 L120,-10 L150,0 L120,10 L-70,12 Z')
    b += line(-60, 0, 130, 0, w=3)
    return group(b, s=0.82)

def pike():
    b = rect(-170, -7, 250, 14, fill=G, rx=6)
    b += lines((-120, -7, -126, 7), (-60, -7, -66, 7), (0, -7, -6, 7))
    b += rect(70, -12, 22, 24, rx=3)
    b += path('M90,-12 Q130,-24 176,0 Q130,24 90,12 Z')
    b += line(96, 0, 168, 0, w=3)
    return group(b, s=0.78)

def shovel():
    b = rect(-140, -7, 170, 14, fill=G, rx=5)
    b += rect(-160, -18, 30, 36, rx=8)
    b += path('M26,-16 L90,-22 Q124,0 90,22 L26,16 Z')
    b += line(40, 0, 100, 0, w=3)
    return group(b, s=0.85)

def wrench():
    b = rect(-110, -12, 160, 24, rx=10)
    b += path('M40,-26 L96,-30 L120,-8 L92,-8 L92,8 L120,8 L96,30 L40,26 Z')
    b += circ(-100, 0, 4, fill=K, w=1)
    b += hatch(-90, -8, 20, 8, 5)
    return group(b, s=1.0)

def claws():
    b = ''
    for dy in (-40, 0, 40):
        b += path(f'M-80,{dy-10} Q20,{dy-40} 100,{dy-30} Q30,{dy-14} -70,{dy+12} Z')
    b += rect(-110, -56, 40, 112, fill=G, rx=12)
    return group(b, rot=-20, s=0.9)

def rifle_bash():
    b = poly([(-120, -20), (20, -16), (20, 20), (-110, 40), (-126, 34)])
    b += hatch(-110, -14, 10, 14, 6)
    b += lines((40, -40, 70, -60), (50, 0, 92, 0), (40, 40, 70, 60), w=7)
    return group(b, rot=-25, s=0.95)


# ------------------------------------------------------------------------------------------------ equipaggiamento
def grenade_frag():
    b = ell(0, 20, 52, 64)
    for y in (-10, 20, 50): b += path(f'M-48,{y} Q0,{y+12} 48,{y}', fill='none', w=4)
    b += line(-18, -40, -18, 82, w=4); b += line(18, -40, 18, 82, w=4)
    b += rect(-18, -62, 36, 22, fill=G, rx=3)
    b += path('M18,-56 Q60,-64 56,-20 L46,-20 Q48,-48 18,-46 Z')     # leva
    b += circ(-30, -60, 12, fill='none', w=5)                         # spoletta
    return group(b, rot=0, dy=0)

def grenade_spiky():
    b = ''
    for i in range(10):
        a = i * 2 * math.pi / 10
        x, y = math.cos(a) * 58, math.sin(a) * 58 + 16
        b += poly([(math.cos(a - 0.2) * 42, math.sin(a - 0.2) * 42 + 16), (x, y), (math.cos(a + 0.2) * 42, math.sin(a + 0.2) * 42 + 16)], w=4)
    b += circ(0, 16, 44)
    b += circ(-14, 4, 6, fill=G, w=3); b += circ(14, 26, 6, fill=G, w=3); b += circ(10, -4, 4, fill=G, w=3)
    b += rect(-12, -50, 24, 22, fill=G, rx=3)
    return group(b, rot=0, s=1.35, dy=-12)

def grenade_piercing():
    b = path('M-34,-30 L34,-30 L34,40 L0,96 L-34,40 Z')
    b += lines((-34, 10, 34, 10), (-34, 40, 34, 40))
    b += rect(-20, -56, 40, 26, fill=G, rx=3)
    b += path('M20,-50 Q54,-56 50,-20 L42,-20 Q42,-44 20,-40 Z')
    return group(b, rot=0, dy=-10)

def grenade_scrap():
    b = rect(-44, -30, 88, 110, rx=8)
    b += lines((-44, 0, 44, 6), (-44, 40, 44, 34))
    for x, y in ((-28, -16), (24, 18), (-20, 56), (28, 62)): b += circ(x, y, 6, fill=G, w=3)
    b += rect(-10, -54, 20, 24, fill=G, rx=3)
    b += path('M0,-54 Q20,-80 40,-70', fill='none', w=4)
    return group(b, rot=8)

def bomb():
    b = ''
    for x in (-40, 0, 40): b += rect(x - 18, -60, 36, 130, rx=12)
    b += rect(-64, -20, 128, 22, fill=G, rx=4); b += rect(-64, 30, 128, 22, fill=G, rx=4)
    b += path('M0,-60 Q20,-100 50,-90', fill='none', w=5)
    b += path('M50,-90 L62,-104 L58,-88 L72,-92 Z', fill=G, w=3)
    return group(b, rot=10, dy=10)

def mine():
    b = ell(0, 30, 92, 36, fill=G)
    b += path('M-92,30 L-92,10 Q0,-34 92,10 L92,30 Q0,66 -92,30 Z')
    b += ell(0, 8, 92, 34)
    b += ell(0, 2, 24, 12, fill=G)
    b += rect(-6, -22, 12, 20, rx=3)
    for i in range(6):
        a = i * math.pi / 3
        b += circ(math.cos(a) * 64, 8 + math.sin(a) * 22, 5, fill=K, w=1)
    return group(b, rot=0)

def ammo_box(crate=False):
    b = poly([(-90, -20), (40, -20), (90, -50), (-40, -50)], fill=G)
    b += rect(-90, -20, 130, 90)
    b += poly([(40, -20), (90, -50), (90, 40), (40, 70)], fill=G)
    if crate:
        b += lines((-90, -20, 40, 70), (-90, 70, 40, -20))
    else:
        for x in (-70, -50, -30, -10, 10):
            b += rect(x, -66, 14, 40, rx=7, fill=W, w=4); b += rect(x, -40, 14, 14, fill=G, w=4)
        b += rect(-40, 10, 40, 18, fill=G, rx=2)
    return group(b, rot=0, dy=10)

def backpack():
    b = rect(-60, -60, 120, 140, rx=24)
    b += rect(-46, 20, 92, 48, fill=G, rx=10)
    b += rect(-30, -84, 60, 30, fill='none', rx=12, w=6)
    b += lines((-46, -20, 46, -20))
    for x in (-24, -8, 8, 24): b += rect(x - 5, -54, 10, 30, rx=5, fill=W, w=3)
    return group(b, rot=0)

def banner():
    b = rect(-70, -100, 10, 210, fill=G, rx=4)
    b += circ(-65, -106, 10)
    b += path('M-60,-90 L80,-90 L80,40 L10,20 L-60,40 Z')
    b += path('M10,-70 L22,-40 L54,-40 L28,-20 L38,10 L10,-8 L-18,10 L-8,-20 L-34,-40 L-2,-40 Z', fill=G, w=4)
    return group(b, rot=0)

def power_pack():
    b = rect(-50, -70, 100, 150, rx=12)
    b += rect(-24, -90, 48, 24, fill=G, rx=4)
    b += path('M10,-46 L-24,14 L0,14 L-12,58 L26,-6 L2,-6 Z', fill=G, w=5)
    return group(b, rot=0)

def cloak():
    b = path('M-30,-90 Q0,-100 30,-90 Q70,10 90,96 Q0,80 -90,96 Q-70,10 -30,-90 Z')
    b += path('M-30,-90 Q-10,-40 -40,90', fill='none', w=4); b += path('M30,-90 Q10,-40 40,90', fill='none', w=4)
    b += ell(0, -86, 34, 14, fill=G)
    for x, y in ((-50, 30), (40, 0), (-10, 50), (56, 60)): b += ell(x, y, 12, 7, fill=G2, w=2)
    return group(b, rot=0)

def scanner():
    b = rect(-60, -80, 120, 150, rx=12)
    b += rect(-44, -64, 88, 70, fill=G, rx=6)
    for r in (16, 30, 44): b += path(f'M{-r},{-4} A{r},{r} 0 0 1 {r},-4', fill='none', w=3)
    b += circ(0, -4, 4, fill=K, w=1)
    b += circ(-24, 36, 9); b += circ(24, 36, 9)
    b += rect(-14, -110, 28, 30, rx=4, fill=G)
    return group(b, rot=0)

def radio():
    b = rect(-70, -40, 140, 110, rx=10)
    b += circ(-30, 14, 26, fill=G); b += circ(-30, 14, 10, fill=W, w=4)
    b += rect(14, -20, 40, 20, fill=G, rx=3)
    b += circ(24, 30, 8); b += circ(46, 30, 8)
    b += line(50, -40, 80, -110, w=6); b += circ(80, -110, 6, fill=K, w=1)
    return group(b, rot=0)

def medkit():
    b = rect(-80, -50, 160, 120, rx=12)
    b += rect(-30, -70, 60, 24, fill='none', rx=8, w=6)
    b += path('M-14,-30 L14,-30 L14,-4 L40,-4 L40,24 L14,24 L14,50 L-14,50 L-14,24 L-40,24 L-40,-4 L-14,-4 Z', fill=G)
    return group(b, rot=0)

def slit_wall():
    b = rect(-100, -60, 200, 130, rx=4)
    b += rect(-60, -20, 120, 18, fill=K, rx=2, w=4)
    for y in (-40, 20, 50):
        off = 0 if y % 2 else 30
        b += line(-100, y, 100, y, w=3)
    for x, y0, y1 in ((-50, -60, -40), (30, -60, -40), (-10, 20, 50), (60, 20, 50), (-70, 50, 70), (10, 50, 70)): b += line(x, y0, x, y1, w=3)
    return group(b, rot=0)

def grugn(bomb_=False):
    b = ell(0, 20, 76, 60)
    b += poly([(-50, -20), (-70, -70), (-24, -36)]); b += poly([(50, -20), (70, -70), (24, -36)])   # orecchie
    b += ell(0, 44, 34, 22, fill=G)
    b += circ(-12, 44, 5, fill=K, w=1); b += circ(12, 44, 5, fill=K, w=1)
    b += circ(-28, 6, 8, fill=K, w=1); b += circ(28, 6, 8, fill=K, w=1)
    b += path('M-34,62 Q-46,82 -30,90', fill='none', w=5); b += path('M34,62 Q46,82 30,90', fill='none', w=5)   # zanne
    if bomb_:
        b += circ(60, -50, 26, fill=G); b += path('M70,-74 Q86,-96 100,-90', fill='none', w=4)
    return group(b, rot=0, dy=6)


# ------------------------------------------------------------------------------------------------ armature
def helmet(kind):
    b = ''
    if kind == 'brodie':                                       # elmetto da trincea
        b += ell(0, 30, 110, 26, fill=G)
        b += path('M-70,30 Q-70,-70 0,-70 Q70,-70 70,30 Z')
        b += path('M-50,0 Q0,-14 50,0', fill='none', w=3)
    elif kind == 'visor':                                      # elmo con visiera
        b += path('M-70,40 L-70,-20 Q-70,-80 0,-80 Q70,-80 70,-20 L70,40 Q0,70 -70,40 Z')
        b += path('M-60,-10 L60,-10 L54,20 L-54,20 Z', fill=G)
        b += line(-56, 5, 56, 5, w=3)
    elif kind == 'great':                                      # grande elmo dell'Ordo
        b += path('M-64,80 L-64,-40 Q-64,-84 0,-84 Q64,-84 64,-40 L64,80 Z')
        b += rect(-50, -16, 100, 12, fill=K, rx=2, w=3)        # feritoia
        b += rect(-6, -4, 12, 70, fill=K, rx=2, w=3)           # croce di prese d'aria
        for y in (30, 50): b += circ(-26, y, 4, fill=K, w=1); b += circ(26, y, 4, fill=K, w=1)
    elif kind == 'power':                                      # elmo delle Vanguard Armor
        b += path('M-60,60 L-70,-10 Q-60,-80 0,-84 Q60,-80 70,-10 L60,60 L20,80 L-20,80 Z')
        b += path('M-50,-20 L-10,-6 L-14,10 L-48,0 Z', fill=G); b += path('M50,-20 L10,-6 L14,10 L48,0 Z', fill=G)
        b += rect(-24, 34, 48, 30, fill=G, rx=6)
        for x in (-12, 0, 12): b += line(x, 38, x, 60, w=3)
        b += line(0, -84, 0, -30, w=4)
    elif kind == 'scrap':                                      # elmo di ferraglia
        b += path('M-80,30 L-66,-40 L-10,-76 L60,-60 L80,30 Z')
        b += lines((-66, -40, 60, -60), (-10, -76, 0, 30))
        for x, y in ((-50, -10), (40, -20), (-20, 10), (60, 10)): b += circ(x, y, 6, fill=G, w=3)
        b += path('M-80,30 L-96,40 L96,40 L80,30', fill=G)
    elif kind == 'carapace':
        b += path('M-70,50 L-70,-20 Q-70,-80 0,-84 Q70,-80 70,-20 L70,50 Z')
        b += path('M-56,-6 L56,-6 L50,22 L-50,22 Z', fill=K, w=3)
        b += path('M0,-84 L0,-10', fill='none', w=3)
        b += rect(-70, 30, 140, 20, fill=G, rx=4)
    else:                                                      # generico
        b += path('M-70,40 Q-70,-80 0,-80 Q70,-80 70,40 Z')
    return group(b, rot=0, dy=6)

def torso(kind):
    b = ''
    base = 'M-60,-80 L-30,-90 Q0,-70 30,-90 L60,-80 L90,-50 L70,-30 L64,90 L-64,90 L-70,-30 L-90,-50 Z'
    if kind == 'fatigues':
        b += path(base)
        b += line(0, -74, 0, 90, w=3)
        b += rect(-46, -40, 30, 26, fill=G, rx=3); b += rect(16, -40, 30, 26, fill=G, rx=3)
        for y in (-20, 10, 40, 70): b += circ(0, y, 3, fill=K, w=1)
    elif kind == 'rags':
        b += path('M-56,-76 L-20,-84 Q0,-70 20,-84 L56,-76 L74,90 L50,80 L30,96 L10,82 L-14,96 L-34,80 L-56,94 L-74,90 Z')
        b += lines((-40, -40, -20, 30), (30, -50, 40, 20), (-10, 40, 10, 60))
    elif kind == 'flak':
        b += path(base, fill=G)
        b += path('M-56,-70 L56,-70 L60,80 L-60,80 Z')
        for y in (-40, -10, 20, 50): b += line(-56, y, 56, y, w=3)
        b += line(0, -70, 0, 80, w=3)
    elif kind == 'plate':
        b += path(base)
        b += path('M-50,-60 Q0,-40 50,-60 L46,20 Q0,40 -46,20 Z', fill=G)
        b += path('M-48,30 Q0,50 48,30', fill='none', w=4); b += path('M-50,56 Q0,76 50,56', fill='none', w=4)
    elif kind == 'carapace':
        b += path(base)
        b += path('M-56,-64 L0,-40 L56,-64 L52,30 L0,50 L-52,30 Z', fill=G)
        b += line(0, -40, 0, 50, w=3)
        b += path('M-40,62 L40,62', fill='none', w=4)
    elif kind == 'power':
        b += path('M-96,-60 L-60,-90 L-20,-76 L20,-76 L60,-90 L96,-60 L84,-20 L60,-24 L56,90 L-56,90 L-60,-24 L-84,-20 Z')
        b += path('M-40,-60 L40,-60 L30,10 L0,24 L-30,10 Z', fill=G)
        b += poly([(0, -50), (14, -24), (0, 4), (-14, -24)], w=4)          # pietra al centro
        for y in (40, 66): b += line(-50, y, 50, y, w=4)
    elif kind == 'teutonic':
        b += path(base)
        b += path('M-12,-60 L12,-60 L10,-20 L44,-24 L44,0 L10,-4 L12,70 L-12,70 L-10,-4 L-44,0 L-44,-24 L-10,-20 Z', fill=K, w=3)   # croce
    elif kind == 'scrap':
        b += path(base, fill=G)
        b += poly([(-58, -66), (10, -70), (0, 10), (-62, 0)]); b += poly([(4, -60), (60, -64), (56, 20), (-4, 26)])
        b += poly([(-60, 20), (50, 30), (60, 86), (-60, 86)])
        for x, y in ((-50, -56), (0, -56), (-50, -8), (48, -50), (48, 10), (-50, 30), (50, 76)): b += circ(x, y, 5, fill=K, w=1)
    elif kind == 'necklace':
        b += path('M-80,-60 Q0,90 80,-60', fill='none', w=6)
        for i in range(7):
            t = (i + 1) / 8
            x = -80 + 160 * t; y = -60 + 150 * 4 * t * (1 - t) * 0.6 + 0
            b += poly([(x - 12, y - 8), (x + 12, y - 10), (x + 8, y + 14), (x - 10, y + 12)], fill=G if i % 2 else W, w=4)
    return group(b, rot=0)

ARM = 'M-34,-80 L34,-80 L30,30 L36,62 Q40,100 0,100 Q-40,100 -36,62 L-30,30 Z'           # braccio fino al pugno
LEG = 'M-38,-104 L38,-104 L32,20 L30,66 L74,84 L74,104 L-34,104 L-32,66 L-36,20 Z'         # gamba fino allo stivale

def arm(kind):
    b = path(ARM, fill=G)
    if kind == 'power':
        b += path('M-70,-50 Q-70,-110 0,-110 Q70,-110 70,-50 L40,-30 L-40,-30 Z')            # spallaccio grosso
        b += path('M-50,-60 Q0,-90 50,-60', fill='none', w=4)
        b += rect(-38, -10, 76, 50, rx=8)                                                   # bracciale
        b += path('M-40,56 L40,56 Q46,104 0,104 Q-46,104 -40,56 Z')                        # guanto
        b += lines((-20, 70, -20, 96), (0, 70, 0, 100), (20, 70, 20, 96), w=3)
    elif kind == 'plate':
        b += path('M-56,-44 Q-56,-100 0,-100 Q56,-100 56,-44 L34,-34 L-34,-34 Z')
        b += path('M-40,-58 Q0,-80 40,-58', fill='none', w=4)
        b += rect(-34, -4, 68, 50, rx=6)
        b += lines((-34, 14, 34, 14), (-34, 30, 34, 30), w=3)
    elif kind == 'scrap':
        b += poly([(-58, -40), (-40, -100), (40, -96), (58, -40)])
        b += poly([(-38, -8), (36, -14), (40, 40), (-34, 46)])
        for x, y in ((-40, -84), (30, -82), (-24, 2), (24, -2), (-20, 34), (26, 30)): b += circ(x, y, 5, fill=K, w=1)
    else:
        b += path('M-50,-46 Q-50,-96 0,-96 Q50,-96 50,-46 L30,-38 L-30,-38 Z')
        b += rect(-34, 0, 68, 40, rx=6)
        b += lines((-34, 14, 34, 14), (-34, 26, 34, 26), w=3)
    return group(b, rot=0)

def leg(kind):
    if kind == 'fatigues':
        b = path(LEG)
        b += rect(-24, -60, 34, 30, fill=G, rx=3)
        b += path('M-32,66 L30,66 L74,84 L74,104 L-34,104 Z', fill=G2)                    # stivale
        b += line(-32, 80, 60, 80, w=3)
        return group(b, rot=0)
    if kind == 'rags':
        b = path('M-38,-104 L38,-104 L34,40 L20,30 L10,56 L-4,36 L-18,58 L-34,40 Z')
        b += lines((-20, -60, -6, -10), (18, -70, 26, 0))
        b += path('M-30,62 L30,62 L70,84 L70,104 L-34,104 Z', fill=G)                    # fasce ai piedi
        b += lines((-30, 76, 40, 76), (-30, 90, 60, 90), w=3)
        return group(b, rot=0)
    b = path(LEG, fill=G)
    if kind == 'power':
        b += rect(-48, -108, 96, 92, rx=16)
        b += circ(0, -6, 26)
        b += path('M-40,16 L40,16 L38,66 L80,84 L80,108 L-44,108 L-40,66 Z')
        b += lines((-40, 46, 38, 46), (-40, 84, 70, 84), w=4)
    elif kind == 'scrap':
        b += poly([(-46, -100), (42, -106), (46, -36), (-44, -30)])
        b += poly([(-40, 6), (42, 2), (40, 60), (-42, 64)])
        for x, y in ((-34, -90), (30, -94), (-30, 14), (30, 12), (-30, 52), (30, 50)): b += circ(x, y, 5, fill=K, w=1)
    else:
        b += rect(-42, -104, 84, 70, rx=10)
        b += circ(0, -12, 20)
        b += path('M-34,10 L34,10 L32,66 L-32,66 Z')
        b += path('M-32,66 L30,66 L74,84 L74,104 L-34,104 Z')
        b += lines((-34, 34, 34, 34), w=3)
    return group(b, rot=0)


# ------------------------------------------------------------------------------------------------ fazioni
def faction(kind):
    b = ''
    if kind == 'vanguard':                       # pietra: cristallo di quarzo a contorno, bianco e nero
        b += poly([(-30, 100), (-62, 40), (-50, -40), (-20, -70), (8, -40), (20, 40), (-4, 100)], fill=W)
        b += poly([(-20, -70), (8, -40), (-8, -30), (-34, -50)], fill=G)
        b += poly([(-8, -30), (8, -40), (20, 40), (6, 50)], fill=G2)
        b += lines((-8, -30, -12, 96), (-34, -50, -46, 50))
        b += poly([(10, 100), (24, 10), (52, -96), (78, -60), (84, 20), (60, 100)], fill=W)
        b += poly([(52, -96), (78, -60), (60, -50), (40, -70)], fill=G)
        b += poly([(60, -50), (78, -60), (84, 20), (66, 30)], fill=G2)
        b += lines((60, -50, 50, 96), (40, -70, 28, 30))
        b += poly([(-90, 100), (-100, 50), (-80, 10), (-64, 40), (-60, 100)], fill=W)
        b += line(-80, 10, -80, 98)
        b += lines((-110, 100, 110, 100), w=SW)
        b += lines((-96, -40, -86, -40), (-91, -45, -91, -35), (90, 50, 100, 50), (95, 45, 95, 55), w=3)   # scintille
        return group(b, rot=0, s=1.0, dy=-4)
    if kind == 'kingship':                       # corona sopra uno scudo
        b += path('M-70,-10 L70,-10 L70,40 Q70,90 0,112 Q-70,90 -70,40 Z')
        b += path('M-70,30 L70,30', fill='none', w=4)
        b += path('M0,-10 L0,108', fill='none', w=4)
        b += hatch(-66, -6, -4, 26, 4)
        b += path('M-76,-26 L-84,-96 L-46,-60 L-20,-110 L0,-62 L20,-110 L46,-60 L84,-96 L76,-26 Z', fill=G)
        b += rect(-80, -36, 160, 22, rx=4)
        for x in (-84, -20, 20, 84): b += circ(x, -104 if abs(x) == 20 else -98, 7)
        return group(b, rot=0, dy=0)
    if kind == 'ordo':                           # scudo con croce nera
        b += path('M-80,-90 L80,-90 L80,10 Q80,80 0,112 Q-80,80 -80,10 Z')
        b += path('M-16,-76 L16,-76 L12,-14 L60,-18 L60,14 L12,10 L16,92 L-16,92 L-12,10 L-60,14 L-60,-18 L-12,-14 Z', fill=K, w=4)
        return group(b, rot=0)
    if kind == 'vrepighs':                       # teschio di cinghiale con zanne
        b += path('M-70,-60 Q0,-110 70,-60 L84,0 Q70,50 40,64 L-40,64 Q-70,50 -84,0 Z')
        b += poly([(-70, -60), (-104, -104), (-52, -82)]); b += poly([(70, -60), (104, -104), (52, -82)])   # orecchie
        b += ell(-34, -16, 18, 14, fill=K, w=2); b += ell(34, -16, 18, 14, fill=K, w=2)
        b += path('M-40,30 Q0,10 40,30 L44,90 Q0,110 -44,90 Z', fill=G)                                  # grugno
        b += ell(-14, 70, 8, 12, fill=K, w=2); b += ell(14, 70, 8, 12, fill=K, w=2)
        b += path('M-44,60 Q-84,70 -78,20 Q-66,52 -40,44 Z'); b += path('M44,60 Q84,70 78,20 Q66,52 40,44 Z')   # zanne
        b += lines((-20, -60, -10, -40), (20, -60, 10, -40), w=3)
        return group(b, rot=0, dy=0)
    return ''


# ------------------------------------------------------------------------------------------------ archetipi
ARCH = {
    'pistol': lambda: pistol(), 'laser_pistol': lambda: pistol('laser'), 'heavy_pistol': lambda: pistol('heavy'),
    'rifle': lambda: rifle(), 'auto_rifle': lambda: rifle(mag='curve', length=0.9),
    'laser_rifle': lambda: rifle('laser', mag='cell'), 'precision_rifle': lambda: rifle('laser', mag='cell', scope=True, length=1.15),
    'laser_mg': lambda: rifle('laser', mag='drum', length=1.15, bipod=True, heavy=True),
    'heavy_rifle': lambda: rifle('heavy_round', mag='box', heavy=True), 'heavy_auto_rifle': lambda: rifle('heavy_round', mag='curve', heavy=True),
    'heavy_mg': lambda: rifle('heavy_round', mag='drum', length=1.2, bipod=True, heavy=True),
    'mg': lambda: rifle(mag='drum', length=1.2, bipod=True, heavy=True),
    'sniper': lambda: rifle(scope=True, length=1.3, bipod=True),
    'shotgun': shotgun, 'blunderbuss': blunderbuss, 'crude_gun': crude_gun, 'heavy_crude_gun': lambda: crude_gun(True),
    'flamer': flamer, 'plasma': plasma, 'fusion': fusion, 'grenade_launcher': lambda: launcher('grenade'),
    'rocket_launcher': lambda: launcher('rocket'), 'gravity_cannon': gravity_cannon, 'psionic_cannon': psionic_cannon, 'turret': turret,
    'knife': knife, 'toothsword': toothsword, 'toothsword_2h': lambda: toothsword(True), 'cleaver': cleaver,
    'heavy_cleaver': lambda: cleaver(True), 'longsword': longsword, 'pike': pike, 'shovel': shovel, 'wrench': wrench,
    'claws': claws, 'rifle_bash': rifle_bash,
    'grenade': grenade_frag, 'nail_grenade': grenade_spiky, 'piercing_grenade': grenade_piercing, 'scrap_grenade': grenade_scrap,
    'bomb': bomb, 'mine': mine, 'ammo_box': ammo_box, 'ammo_crate': lambda: ammo_box(True), 'backpack': backpack,
    'banner': banner, 'power_pack': power_pack, 'cloak': cloak, 'scanner': scanner, 'radio': radio, 'medkit': medkit,
    'slit_wall': slit_wall, 'grugn': grugn, 'grugn_bomb': lambda: grugn(True),
}
for k in ('brodie', 'visor', 'great', 'power', 'scrap', 'carapace'): ARCH['helmet_' + k] = (lambda k=k: helmet(k))
for k in ('fatigues', 'rags', 'flak', 'plate', 'carapace', 'power', 'teutonic', 'scrap', 'necklace'): ARCH['torso_' + k] = (lambda k=k: torso(k))
for k in ('guard', 'plate', 'power', 'scrap'): ARCH['arm_' + k] = (lambda k=k: arm(k))
for k in ('fatigues', 'rags', 'plate', 'power', 'scrap'): ARCH['leg_' + k] = (lambda k=k: leg(k))

ITEM_ARCH = {
    # armi da fuoco
    'auto_pistol_standard': 'pistol', 'footman_auto_pistol': 'pistol', 'kragh_pistol': 'pistol', 'pighcib_pistol': 'pistol',
    'tusker_crude_pistol': 'pistol', 'cadet_high_power_pistol': 'laser_pistol', 'candidate_impulse_pistol': 'laser_pistol',
    'footman_impulse_pistol': 'laser_pistol', 'impulse_pistol_standard': 'laser_pistol', 'levy_impulse_pistol': 'laser_pistol',
    'defender_heavy_round_pistol': 'heavy_pistol', 'explorer_heavy_round_pistol': 'heavy_pistol',
    'auto_rifle_mg': 'laser_mg', 'auto_rifle_standard': 'auto_rifle', 'candidate_auto_rifle': 'auto_rifle', 'footman_auto_rifle': 'auto_rifle',
    'levy_auto_rifle': 'auto_rifle', 'raider_burst_rifle': 'auto_rifle', 'zhelvar_rifle': 'rifle',
    'cadet_high_power_rifle': 'precision_rifle', 'footman_precision_rifle': 'precision_rifle', 'impulse_rifle_aurel': 'laser_rifle',
    'candidate_impulse_rifle': 'laser_rifle', 'footman_impulse_rifle': 'laser_rifle', 'levy_impulse_rifle': 'laser_rifle',
    'recruit_impulse_rifle': 'laser_rifle', 'raider_beam_rifle': 'laser_rifle',
    'candidate_shotgun': 'shotgun', 'combat_shotgun_standard': 'shotgun', 'explorer_shotgun': 'shotgun', 'footman_shotgun': 'shotgun',
    'pighcib_blunderbuss': 'blunderbuss', 'explorer_sniper': 'sniper',
    'defender_auto_heavy_round_rifle': 'heavy_auto_rifle', 'konrad_auto_heavy_round_rifle': 'heavy_auto_rifle',
    'defender_heavy_round_rifle': 'heavy_rifle', 'explorer_heavy_round_rifle': 'heavy_rifle', 'heavy_round_rifle_ordo': 'heavy_rifle',
    'defender_heavy_round_mg': 'heavy_mg', 'explorer_heavy_round_mg': 'heavy_mg', 'heavy_machinegun_standard': 'mg',
    'gunner_crude_rifle': 'crude_gun', 'gunner_heavy_crude_gun': 'heavy_crude_gun', 'tusker_heavy_crude_gun': 'heavy_crude_gun',
    'flamer_standard': 'flamer', 'footman_flamer': 'flamer', 'footman_discharge_rifle': 'plasma', 'plasma_rifle_standard': 'plasma',
    'raider_discharge_rifle': 'plasma', 'footman_fusion_rifle': 'fusion', 'fusion_rifle_standard': 'fusion',
    'footman_grenade_launcher': 'grenade_launcher', 'gunner_rocket_launcher': 'rocket_launcher',
    'gravity_cannon': 'gravity_cannon', 'psionic_cannon': 'psionic_cannon', 'levy_impulse_cannon': 'turret',
    # armi bianche
    'candidate_toothsword': 'toothsword_2h', 'meatshield_pike': 'pike', 'tusker_heavy_cleaver': 'heavy_cleaver',
    'konrad_longsword': 'longsword', 'entrenching_shovel_1h': 'shovel', 'pighcib_wrench': 'wrench', 'raider_rifle_bash': 'rifle_bash',
    # equipaggiamento
    'cadet_ammo_crate': 'ammo_crate', 'pighcib_ammo_box': 'ammo_box', 'gunner_ammo_backpack': 'backpack',
    'defender_house_banner': 'banner', 'defender_power_pack': 'power_pack', 'explorer_camo_cloak': 'cloak',
    'explorer_scanner': 'scanner', 'footman_field_radio': 'radio', 'recruit_medkit': 'medkit', 'raider_slit_wall': 'slit_wall',
    'raider_demolition_bomb': 'bomb', 'tusker_mines': 'mine', 'tusker_nail_grenade': 'nail_grenade',
    'defender_piercing_grenade': 'piercing_grenade', 'gunner_scrap_grenade': 'scrap_grenade',
    'grugn_companion': 'grugn', 'grugn_bomb': 'grugn_bomb',
    # armature sbloccabili (aspetti trasmogrificabili)
    'head_defender_assault': 'helmet_power', 'waist_defender_drape': 'cloak',
}

def arch_of(item_id, kind, j):
    if item_id in ITEM_ARCH: return ITEM_ARCH[item_id]
    i = item_id
    if kind == 'armor':
        loc = (j.get('location') or '').lower()
        style = ('power' if any(s in i for s in ('defender', 'humilitatis')) else
                 'scrap' if any(s in i for s in ('scrap', 'feral', 'gunner', 'tusker', 'raider')) else
                 'rags' if 'rags' in i else 'fatigues' if 'fatigues' in i or 'training' in i else
                 'carapace' if 'carapace' in i else 'plate')
        if 'head' in loc:
            return ('helmet_great' if 'humilitatis' in i else 'helmet_power' if 'defender' in i else 'helmet_scrap' if style == 'scrap' or 'pighcib' in i else
                    'helmet_carapace' if 'carapace' in i else 'helmet_visor' if any(s in i for s in ('visor', 'veteran', 'footman_helmet')) else 'helmet_brodie')
        if 'torso' in loc:
            return ('torso_teutonic' if 'humilitatis' in i else 'torso_necklace' if 'collana' in i else 'torso_power' if 'defender' in i else
                    'torso_rags' if 'rags' in i else 'torso_fatigues' if 'fatigues' in i or 'training' in i else 'torso_carapace' if 'carapace' in i else
                    'torso_scrap' if style == 'scrap' else 'torso_flak' if 'flak' in i or 'recruit' in i else 'torso_plate')
        if 'arm' in loc:
            return 'arm_power' if style == 'power' else 'arm_scrap' if style == 'scrap' else 'arm_plate' if style in ('plate', 'carapace') or 'veteran' in i else 'arm_guard'
        return ('leg_power' if style == 'power' else 'leg_scrap' if style == 'scrap' else 'leg_rags' if style == 'rags' else
                'leg_fatigues' if style == 'fatigues' else 'leg_plate')
    if 'claws' in i: return 'claws'
    if kind == 'melee':
        if 'knife' in i: return 'knife'
        if 'toothsword' in i: return 'toothsword'
        if 'cleaver' in i or 'axe' in i: return 'cleaver'
        return 'longsword'
    if kind == 'equipment':
        if 'frag' in i or 'grenade' in i: return 'grenade'
        return 'ammo_box'
    if 'pistol' in i: return 'pistol'
    return 'rifle'


def render(svg_text, png_path, size=128):
    os.makedirs(os.path.dirname(png_path), exist_ok=True)
    subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), '-o', png_path], input=svg_text.encode(), check=True)


def main():
    save_svg = '--svg' in sys.argv
    made = {}
    for f in ('kingship', 'vanguard', 'ordo', 'vrepighs'):
        s = svg(faction(f))
        render(s, os.path.join(OUT, 'Factions', f + '.png'), 128)
        if save_svg: open(os.path.join(HERE, 'svg', 'faction_' + f + '.svg'), 'w').write(s)
    cache = {}
    items = []
    for fpath in sorted(glob.glob(os.path.join(GD, 'Weapons', '*', '*.json'))):
        j = json.load(open(fpath, encoding='utf-8')); folder = os.path.basename(os.path.dirname(fpath))
        kind = 'melee' if folder.startswith('Melee') else 'equipment' if folder == 'Equipment' else 'ranged'
        items.append((j['id'], kind, j))
    for fpath in sorted(glob.glob(os.path.join(GD, 'Armors', '**', '*.json'), recursive=True)):
        j = json.load(open(fpath, encoding='utf-8')); items.append((j['id'], 'armor', j))
    for iid, kind, j in items:
        a = arch_of(iid, kind, j)
        if a not in cache: cache[a] = svg(ARCH[a]())
        render(cache[a], os.path.join(OUT, 'Items', iid + '.png'), 128)
        made[iid] = a
    if save_svg:
        os.makedirs(os.path.join(HERE, 'svg'), exist_ok=True)
        for a, s in cache.items(): open(os.path.join(HERE, 'svg', a + '.svg'), 'w').write(s)
    # foglio di prova: tutti gli archetipi su fondo chiaro e scuro
    names = sorted(ARCH)
    cols = 10
    rows = (len(names) + 4 + cols - 1) // cols
    cell = 128
    tiles = ''
    for idx, a in enumerate(['@kingship', '@vanguard', '@ordo', '@vrepighs'] + names):
        x, y = (idx % cols) * cell, (idx // cols) * cell
        body = faction(a[1:]) if a.startswith('@') else ARCH[a]()
        bg = '#2a2724' if (idx // cols) % 2 else '#e9e4d8'
        tiles += f'<g transform="translate({x} {y})"><rect width="{cell}" height="{cell}" fill="{bg}"/><g transform="scale(0.5)">{body}</g>' \
                 f'<text x="4" y="{cell-4}" font-size="10" fill="#c33" font-family="sans-serif">{a}</text></g>'
    sheet = f'<svg xmlns="http://www.w3.org/2000/svg" width="{cols*cell}" height="{rows*cell}">{tiles}</svg>'
    subprocess.run(['rsvg-convert', '-o', os.path.join(HERE, 'sheet.png')], input=sheet.encode(), check=True)
    with open(os.path.join(OUT, 'icons.json'), 'w', encoding='utf-8') as f:
        json.dump({'comment': "Icona usata da ogni oggetto (archetipo di Tools/Icons/icons.py). Generato: non modificare a mano.", 'items': made}, f, ensure_ascii=False, indent=1, sort_keys=True)
    print(f'[icone] {len(made)} oggetti, {len(cache)} disegni, 4 fazioni -> {OUT}')


if __name__ == '__main__':
    main()

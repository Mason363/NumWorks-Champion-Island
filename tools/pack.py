#!/usr/bin/env python3
"""Packs the extracted Doodle Champion Island Games into src/data.bin + src/data.h.

Inputs: build/ex/*.json from tools/extract.js, the doodle's sprite sheets and
kitsune20.js (for the image table and the texts). Everything the calculator
needs goes in one blob:

- palettes: each sheet's 256 colours as RGB565 plus alpha;
- sprites: every image the libraries place, as 8-bit palette indices. Small
  ones are packed in LZMA "banks" of at most BANK bytes, decoded into a cache
  on demand; large backgrounds are LZMA "streams" decoded row by row into the
  background layer. Both use a dictionary of at most DICT bytes. A very tall
  stream is cut into strips of about STRIP_BYTES compressed, each its own LZMA
  stream, so that a row deep down is reached without decoding every row above.
- symbols: bitmaps, shapes, texts and clips. A clip's timeline is a list of
  keyframes per child slot, labels, frame scripts (stop, dispatchEvent) and its
  components (the Animate "this.T = {...}" data);
- strings, quest conditions compiled to a small stack program, dialogue trees.

    python3 tools/pack.py [DOODLE_DIR]
"""
import json
import lzma
import math
import os
import re
import struct
import sys
from collections import Counter, OrderedDict, defaultdict

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, '../../../potherca-blog/google-doodle-champion-island'))
EX = os.path.join(ROOT, 'build/ex')
BANK = 16384          # decoded size of a sprite bank at most
DICT = 8192           # LZMA dictionary: the decoder's ring buffer
STREAM_MIN = 20000    # sprites with more pixels than this are streamed backgrounds
STRIP_BYTES = 8000    # a stream taller than STRIP_MIN_H starts afresh once a strip compresses to this much,
STRIP_MIN_H = 512     # so that a row deep down costs no more decoding than that (the climbing vista)

SHEETS = ['preload-sprite.png', 'shared-sprite.png', 'cutscene-sprite.png', 'overworld-sprite.png', 'archery-sprite.png',
          'climbing-sprite.png', 'marathon-sprite.png', 'pingpong-sprite.png', 'rugby-sprite.png', 'skate-sprite.png', 'swim-sprite.png']
LIBS = [('dialog', 'B6B0DC09A20D455BAB815F8FE24BCF08'), ('hud', '0E30EA04F8F04E9B8B08CAC42EEA149E'),
        ('loading', '348D233EE4ED48398F13A42B3BD73D9C'), ('menus', '1754741A4A3841C2AB73BD915D793487'),
        ('video', '462CEA9764EE4C8D86AB0BDFEAEB1BF9'), ('archery', '1F5F819ABC834FD88AEB5AFCEFF8263B'),
        ('climbing', '7B2C344CB74B48B4AFAEBCF1D033F55A'), ('cutscene', '641FECB93B7041208934B77EA1084BE4'),
        ('interior', '61DFBEFE7BBA42719310DD5484391B9B'), ('marathon', '6A005E6C90D74E9A971A793C2ECC526F'),
        ('overworld', '6B325686132B46298EE25D130F14A658'), ('pingpong', '53A3DF4EA6694858812E8CA9B4AB07DC'),
        ('rugby', '6CC3977E663B4BA1A079CF41822948DB'), ('skate', '8AB206B0C8EC450BA137F5272C53C5F0'),
        ('swim', '81A4DDA62E6C4693B3A00E8A0484897E')]

# ------------------------------------------------------------------ inputs
KITSUNE = open(os.path.join(SRC, 'kitsune20.js'), encoding='utf-8').read()


def image_table():
    """kitsune20.js maps each exported image to [sheet, x, y, w, h]."""
    named = {m.group(1): tuple(int(m.group(i)) for i in range(2, 7))
             for m in re.finditer(r'([\w$]+)\s*=\s*\[\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\]', KITSUNE)}
    table = {}
    for m in re.finditer(r'V\.set\("animate_exports/([^"]+)", (\[[^\]]*\]|[\w$]+)\)', KITSUNE):
        v = m.group(2)
        table[m.group(1)] = tuple(int(x) for x in re.findall(r'-?\d+', v)) if v.startswith('[') else named[v]
    return table


VTAB = image_table()
LIB = {}
for name, lid in LIBS:
    d = json.load(open(os.path.join(EX, lid + '.json')))
    man = {k: v.split('?')[0] for k, v in d['manifest']}
    for s in d['syms'].values():
        if s.get('t') == 'b' and s.get('img') in man:
            s['sheet'] = VTAB.get(man[s['img']])
    LIB[name] = d

msg_text = open(os.path.join(SRC, 'messages.en-GB.nocache.json'), encoding='utf-8').read()
MESSAGES = json.loads(msg_text[msg_text.find('{'):])
DIALOG = json.load(open(os.path.join(EX, 'dialog.json')))
# the calculator's keys in the instructions: OK acts, Back cancels
KEYS = [('Spacebar', 'OK'), ('Space ', 'OK '), ('space', 'OK'), ('backspace', 'Back'), ('Direction keys', 'Arrow keys'),
        ('Click on the red gate', 'Go to the red gate'), ('by clicking the sport icons on the main menu', 'by choosing a sport on the map')]
for _k in ('PRESS_SPACE', 'ARIA_BILLBOARD_DOCK', 'SPACEBAR', 'BACKSPACE', 'questTrainTrackscomplete', 'townspeoplewhatsft') + tuple(
        k for k in MESSAGES if k.startswith('TUT_')):
    for a_, b_ in KEYS:
        MESSAGES[_k] = MESSAGES[_k].replace(a_, b_)

# ------------------------------------------------------------------ blob helpers
HEADER_WORDS = 30
STATS = {}


class Blob:
    def __init__(self):
        self.b = bytearray(4 * HEADER_WORDS)   # the header, filled in at the end

    def align(self, n=4):
        while len(self.b) % n:
            self.b.append(0)

    def add(self, data, align=4, cat='?'):
        STATS[cat] = STATS.get(cat, 0) + len(data)
        self.align(align)
        off = len(self.b)
        self.b += data
        return off


blob = Blob()
strings = OrderedDict()


def sid(s):
    """Interned string id (UTF-8, zero-terminated)."""
    if s is None:
        return 0xFFFF
    s = str(s)
    if s not in strings:
        strings[s] = len(strings)
    return strings[s]


def cname(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s.replace('$', '_S_'))


def lzma_raw(data, dict_size):
    ds = max(4096, min(dict_size, DICT))
    return lzma.compress(bytes(data), format=lzma.FORMAT_RAW, filters=[
        {'id': lzma.FILTER_LZMA1, 'dict_size': ds, 'lc': 0, 'lp': 0, 'pb': 0, 'preset': 9 | lzma.PRESET_EXTREME}])


# ------------------------------------------------------------------ palettes
sheet_px, sheet_alpha, pal_rgb = {}, {}, {}
for i, f in enumerate(SHEETS):
    im = Image.open(os.path.join(SRC, f))
    assert im.mode == 'P'
    sheet_px[i] = np.array(im)
    a = np.full(256, 255, np.uint8)
    tr = im.info.get('transparency')
    if isinstance(tr, bytes):
        a[:len(tr)] = np.frombuffer(tr, np.uint8)
    elif isinstance(tr, int):
        a[tr] = 0
    pal = (im.getpalette() + [0] * 768)[:768]
    sheet_alpha[i] = a
    pal_rgb[i] = [(pal[3 * j], pal[3 * j + 1], pal[3 * j + 2]) for j in range(256)]

# ------------------------------------------------------------------ transforms
def matrix(tr):
    """CreateJS DisplayObject.getMatrix: a, b, c, d, tx, ty."""
    x, y = tr.get('x', 0), tr.get('y', 0)
    sx, sy = tr.get('sx', 1), tr.get('sy', 1)
    r = tr.get('r', 0)
    kx, ky = tr.get('kx', 0), tr.get('ky', 0)
    rx, ry = tr.get('rx', 0), tr.get('ry', 0)
    D = math.pi / 180
    if r % 360:
        cos, sin = math.cos(r * D), math.sin(r * D)
    else:
        cos, sin = 1.0, 0.0
    # Matrix2D.appendTransform(x, y, sx, sy, r, kx, ky, rx, ry)
    if kx or ky:
        kx *= D
        ky *= D
        a1, b1, c1, d1 = math.cos(ky), math.sin(ky), -math.sin(kx), math.cos(kx)
        a2, b2, c2, d2 = cos * sx, sin * sx, -sin * sy, cos * sy
        a = a1 * a2 + c1 * b2
        b = b1 * a2 + d1 * b2
        c = a1 * c2 + c1 * d2
        d = b1 * c2 + d1 * d2
    else:
        a, b, c, d = cos * sx, sin * sx, -sin * sy, cos * sy
    tx = x - (rx * a + ry * c)
    ty = y - (rx * b + ry * d)
    return (a, b, c, d, tx, ty)



def mat_mul(m, n):
    a, b, c, d, tx, ty = m
    A, B, C, D, TX, TY = n
    return (a * A + c * B, b * A + d * B, a * C + c * D, b * C + d * D, a * TX + c * TY + tx, b * TX + d * TY + ty)


# ------------------------------------------------------------------ masks
def mask_box(m):
    """The rectangle a mask's path is, in its parent's space (x0, y0, x1, y1), or None."""
    if [op for op, _ in m['g'] if op not in ('P', 'Z')] != ['M', 'L', 'L', 'L']:
        return None
    a, b, c, d, tx, ty = matrix(m['tr'])
    if b or c:
        return None
    pts = [(round(a * p.get('x', 0) + tx, 3), round(d * p.get('y', 0) + ty, 3)) for op, p in m['g'] if op in ('M', 'L')]
    if any((p[0] == q[0]) == (p[1] == q[1]) for p, q in zip(pts, pts[1:] + pts[:1])):
        return None
    return min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)


# A mask is a clipping path. A bitmap that a rectangle masks, the same on every
# frame (Lucky's climbing statue shows only the pedestal of the champion's),
# becomes the part of its image the mask shows: a symbol of its own, after all
# the others. Masks that move are not kept (the child is drawn whole).
CUTS = []   # (lib, name) of the cut bitmaps
for lib, _ in LIBS:
    syms = LIB[lib]['syms']
    for k, s in list(syms.items()):
        masked = defaultdict(list)
        for fr in s.get('frames') or []:
            for c in fr:
                if c.get('mask'):
                    masked[c['i']].append(c)
        for cs in masked.values():
            c = cs[0]
            still = all(json.dumps([o.get('sym'), o['tr'], o['mask']]) == json.dumps([c.get('sym'), c['tr'], c['mask']]) for o in cs)
            sheet = syms.get(c.get('sym'), {}).get('sheet') if c['t'] == 'b' else None
            box = mask_box(c['mask'])
            a, b, cc, d, tx, ty = matrix(c['tr'])
            if not still or not sheet or not box or b or cc or a <= 0 or d <= 0:
                continue
            sh, x, y, w, h = sheet
            u0, v0 = max(0, math.floor((box[0] - tx) / a + .5)), max(0, math.floor((box[1] - ty) / d + .5))
            u1, v1 = min(w, math.floor((box[2] - tx) / a + .5)), min(h, math.floor((box[3] - ty) / d + .5))
            if u0 >= u1 or v0 >= v1:
                continue
            name = '%s.%s' % (k, c['sym'])
            syms[name] = {'t': 'b', 'sheet': (sh, x + u0, y + v0, u1 - u0, v1 - v0)}
            CUTS.append((lib, name))
            for o in cs:
                del o['mask']
                o['sym'] = name
                o['tr'] = dict(o['tr'], x=round(o['tr'].get('x', 0) + a * u0, 3), y=round(o['tr'].get('y', 0) + d * v0, 3))

# ------------------------------------------------------------------ symbols
SYMS = []            # (lib, name)
SYM_ID = {}
for name, _ in LIBS:
    for k in LIB[name]['syms']:
        if (name, k) not in CUTS:
            SYM_ID[(name, k)] = len(SYMS)
            SYMS.append((name, k))
for key in CUTS:
    SYM_ID[key] = len(SYMS)
    SYMS.append(key)

# sprites: every bitmap symbol with an image
SPR_ID, SPR = {}, []   # key (sheet,x,y,w,h) -> id
sym_sprite = {}
for (lib, k) in SYMS:
    s = LIB[lib]['syms'][k]
    if s.get('t') == 'b' and s.get('sheet'):
        key = tuple(s['sheet'])
        if key not in SPR_ID:
            SPR_ID[key] = len(SPR)
            SPR.append({'key': key, 'libs': set()})
        SPR[SPR_ID[key]]['libs'].add(lib)
        sym_sprite[(lib, k)] = SPR_ID[key]

# ------------------------------------------------------------------ shapes & texts
def parse_color(style):
    if not style or style in ('null', 'undefined'):
        return None
    style = style.strip()
    m = re.match(r'#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?$', style)
    if m:
        v = int(m.group(1), 16)
        a = int(m.group(2), 16) if m.group(2) else 255
        return (v >> 16, v >> 8 & 255, v & 255, a)
    m = re.match(r'#([0-9a-fA-F]{3})$', style)
    if m:
        r, g, b = (int(ch * 2, 16) for ch in m.group(1))
        return (r, g, b, 255)
    m = re.match(r'rgba?\(([^)]*)\)', style)
    if m:
        p = [float(v) for v in m.group(1).split(',')]
        return (int(p[0]), int(p[1]), int(p[2]), int(round((p[3] if len(p) > 3 else 1) * 255)))
    return None


def flatten_path(ins):
    """Graphics instructions -> [(fill rgba or None, [polygon, ...])]. CreateJS
    lists a path's commands first, then its fill (or stroke)."""
    shapes, polys, poly = [], [], []
    cx = cy = 0.0

    def close():
        nonlocal poly
        if len(poly) >= 2:
            polys.append(poly)
        poly = []

    def emit(fill):
        nonlocal polys
        close()
        if polys:
            shapes.append((fill, polys))
        polys = []

    for kind, d in ins:
        if kind == 'P':
            emit(None)
        elif kind == 'F':
            emit(parse_color(d.get('style')))
        elif kind in ('S', 'SS'):
            close()
        elif kind == 'M':
            close()
            cx, cy = d['x'], d['y']
            poly = [(cx, cy)]
        elif kind == 'L':
            cx, cy = d['x'], d['y']
            poly.append((cx, cy))
        elif kind == 'Q':
            x0, y0 = cx, cy
            for i in range(1, 9):
                t = i / 8
                poly.append(((1 - t) ** 2 * x0 + 2 * (1 - t) * t * d['cpx'] + t * t * d['x'],
                             (1 - t) ** 2 * y0 + 2 * (1 - t) * t * d['cpy'] + t * t * d['y']))
            cx, cy = d['x'], d['y']
        elif kind == 'B':
            x0, y0 = cx, cy
            for i in range(1, 11):
                t = i / 10
                mt = 1 - t
                poly.append((mt ** 3 * x0 + 3 * mt * mt * t * d['cp1x'] + 3 * mt * t * t * d['cp2x'] + t ** 3 * d['x'],
                             mt ** 3 * y0 + 3 * mt * mt * t * d['cp1y'] + 3 * mt * t * t * d['cp2y'] + t ** 3 * d['y']))
            cx, cy = d['x'], d['y']
        elif kind == 'Z':
            close()
        elif kind in ('R', 'RR'):
            close()
            x, y, w, h = d['x'], d['y'], d['w'], d['h']
            polys.append([(x, y), (x + w, y), (x + w, y + h), (x, y + h)])
        elif kind == 'C':
            close()
            polys.append([(d['x'] + d['radius'] * math.cos(i * math.pi / 8), d['y'] + d['radius'] * math.sin(i * math.pi / 8)) for i in range(16)])
        elif kind == 'E':
            close()
            x, y, w, h = d['x'], d['y'], d['w'], d['h']
            polys.append([(x + w / 2 + w / 2 * math.cos(i * math.pi / 8), y + h / 2 + h / 2 * math.sin(i * math.pi / 8)) for i in range(16)])
    emit(None)
    return shapes


def enc_shape(ins):
    out = bytearray()
    shapes = flatten_path(ins)
    out.append(len(shapes))
    for fill, polys in shapes:
        r, g, b, a = fill if fill else (0, 0, 0, 0)
        out += struct.pack('<BBBBB', r, g, b, a, len(polys))
        for p in polys:
            out += struct.pack('<H', len(p))
            for (x, y) in p:
                out += struct.pack('<hh', int(round(x * 4)), int(round(y * 4)))
    return bytes(out)


def enc_text(c):
    font = c.get('font') or ''
    m = re.search(r'(\d+(?:\.\d+)?)px', font)
    size = int(round(float(m.group(1)))) if m else 10
    col = parse_color(c.get('color')) or (0, 0, 0, 255)   # CreateJS draws texts without a colour black
    align = {'left': 0, 'start': 0, 'center': 1, 'right': 2, 'end': 2}.get(c.get('align') or 'left', 0)
    base = {'top': 0, 'hanging': 0, 'middle': 1, 'alphabetic': 2, 'ideographic': 2, 'bottom': 3}.get(c.get('base') or 'top', 0)
    return struct.pack('<HBBBBBBhh', sid(c.get('text') or ''), size, col[0], col[1], col[2], align, base,
                       int(round((c.get('lw') or 0) * 4)), int(round((c.get('lh') or 0) * 4)))


# ------------------------------------------------------------------ worlds
# The island map (the overworld's "jqa") is huge: 24 regions holding some 9000
# display objects. On the calculator its scenery (ground, trees, houses,
# fences...) is not in the display tree: world.c paints it into the background
# layer as the camera moves, from a table of "statics" in the doodle's draw
# order (drawOrderOverride, else y). What moves, talks or reacts stays a clip,
# moved up from its region to the map (the doodle does the same when a region
# comes into view), and is instantiated only near the camera.
WORLDS = [('overworld', 'jqa')]
BAKE_COMPS = {'boundable', 'collidable', 'drawOrderOverride', 'untraversable', 'tileBackground'}
BAND_W = 128          # big ground images are stored as columns of this width
WORLD_CELL = 128      # spatial index cell
WORLD = []            # per world: dict
banded = set()        # sprites stored as column bands
world_drop = set()    # (lib, sym) with no clip data (regions and what only they use)
world_warn = Counter()


def content_static(lib, k, depth=0):
    """No animation anywhere below (the clip always looks the same)."""
    s = LIB[lib]['syms'].get(k)
    if not s or s.get('t') in ('b', 's'):
        return True
    if s.get('t') != 'm' or depth > 20:
        return False
    if len(s['frames']) > 1 and any(f != s['frames'][0] for f in s['frames'][1:]):
        return False
    return all(content_static(lib, c['sym'], depth + 1) for c in s['frames'][0] if c.get('sym'))


def flatten_draws(lib, k, M, alpha, frame, out, depth=0):
    """The bitmaps a symbol shows at a frame, placed in map space: (sprite, M, alpha)."""
    s = LIB[lib]['syms'].get(k)
    if not s or depth > 20 or alpha <= 0:
        return
    if s.get('t') == 'b':
        if (lib, k) in sym_sprite:
            out.append((sym_sprite[(lib, k)], M, alpha))
        return
    if s.get('t') != 'm':
        world_warn['non-bitmap symbol ' + str(s.get('t'))] += 1
        return
    fr = s['frames'][min(frame, len(s['frames']) - 1)] if s['frames'] else []
    for c in fr:
        tr = c.get('tr', {})
        if tr.get('v', 1) == 0:
            continue
        a = alpha * tr.get('a', 1)
        if a <= 0:
            continue
        if c['t'] == 's':
            world_warn['shape in scenery'] += 1
            continue
        if c['t'] == 'x':
            world_warn['text in scenery'] += 1
            continue
        if not c.get('sym'):
            continue
        cs = LIB[lib]['syms'].get(c['sym'], {})
        f = 0
        if cs.get('t') == 'm':
            mode = c.get('mode')
            f = (c.get('sp') or 0) + (frame if mode == 'synched' else 0) if mode in ('single', 'synched') else 0
            nfr = len(cs['frames']) or 1
            f %= nfr
        flatten_draws(lib, c['sym'], mat_mul(M, matrix(tr)), a, f, out, depth + 1)


def sym_bounds(lib, k, depth=0):
    """CreateJS getBounds at frame 0: a bitmap's size, else the union of the visible children's bounds."""
    s = LIB[lib]['syms'].get(k, {})
    if s.get('t') == 'b':
        if (lib, k) in sym_sprite:
            _, _, _, w, h = SPR[sym_sprite[(lib, k)]]['key']
            return (0, 0, w, h)
        return None
    if s.get('t') == 's':
        pts = [p for _, polys in flatten_path(s.get('g') or []) for poly in polys for p in poly]
        if not pts:
            return None
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))
    if s.get('t') != 'm' or depth > 12 or not s.get('frames'):
        return None
    box = None
    for c in s['frames'][0]:
        tr = c.get('tr', {})
        if tr.get('v', 1) == 0:
            continue
        if c['t'] == 's':
            pts = [p for _, polys in flatten_path(c.get('g') or []) for poly in polys for p in poly]
            r = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts) - min(p[0] for p in pts),
                 max(p[1] for p in pts) - min(p[1] for p in pts)) if pts else None
        elif c.get('sym'):
            r = sym_bounds(lib, c['sym'], depth + 1)
        else:
            r = None
        if not r:
            continue
        b = aabb(matrix(tr), r)
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    return (box[0], box[1], box[2] - box[0], box[3] - box[1]) if box else None


def aabb(M, r):
    x, y, w, h = r
    pts = [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]
    xs = [M[0] * px + M[2] * py + M[4] for px, py in pts]
    ys = [M[1] * px + M[3] * py + M[5] for px, py in pts]
    return (min(xs), min(ys), max(xs), max(ys))


def orientation(M):
    """Draw flags of a matrix that only mirrors or turns by 90 degrees (scale ~1):
    1 mirror x, 2 mirror y, 4 transpose (then the mirrors apply on screen)."""
    a, b, c, d = M[:4]
    near = lambda v, t: abs(v - t) < 1e-2
    if near(b, 0) and near(c, 0) and near(abs(a), 1) and near(abs(d), 1):
        return (1 if a < 0 else 0) | (2 if d < 0 else 0)
    if near(a, 0) and near(d, 0) and near(abs(b), 1) and near(abs(c), 1):
        return 4 | (1 if c < 0 else 0) | (2 if b < 0 else 0)
    return None


def tr_of(M, tr):
    """A child transform with matrix M (CreateJS skew form), keeping alpha, visibility, name."""
    a, b, c, d, tx, ty = M
    out = {'x': tx, 'y': ty}
    sx, sy = math.hypot(a, b), math.hypot(c, d)
    ky, kx = math.degrees(math.atan2(b, a)), math.degrees(math.atan2(-c, d))
    if abs(sx - 1) > 1e-6:
        out['sx'] = sx
    if abs(sy - 1) > 1e-6:
        out['sy'] = sy
    if abs(kx) > 1e-6 or abs(ky) > 1e-6:
        if abs(kx - ky) < 1e-6:
            out['r'] = ky
        else:
            out['kx'], out['ky'] = kx, ky
    for f in ('a', 'v', 'nm'):
        if f in tr:
            out[f] = tr[f]
    return out


# property names the game code reads (b.title, b.Jw.zK...): Closure renamed most
# of Animate's instance properties, so only those the code uses are kept
_code = KITSUNE.split('\n')
USED_PROPS = set(re.findall(r'\.([A-Za-z_$][\w$]*)', '\n'.join(_code[2780:4830] + _code[24600:]))) - {'shape'}


def child_name(c):
    """The names code can find a child by: its instance name and/or the property
    it is kept in, as "name|property" when both."""
    nm, pk = c['tr'].get('nm'), c.get('pk')
    if nm == 'hitArea':   # touch areas: the calculator has keys
        nm = None
    if pk not in USED_PROPS or pk == nm or pk == 'hitArea':
        pk = None
    if nm and pk:
        return nm + '|' + pk
    return nm or pk


# live children whose first pictures never change (a house under its door):
# those pictures are baked into the layer, and the symbol loses them
LEAD_COMPS = {'boundable', 'collidable', 'jumpToFrameOnTrigger', 'trigger', 'scenePortal', 'untraversable'}
LEAD_BAKED = {}


def _world_only_uses():
    """(lib, sym) used as a child only by maps and their regions."""
    ok = {}
    for lib_, _ in LIBS:
        L = LIB[lib_]['syms']
        for k, s_ in L.items():
            T = s_.get('T') or {}
            is_world = 'map' in T or 'region' in T
            for fr in s_.get('frames') or []:
                for c in fr:
                    if c.get('sym'):
                        key = (lib_, c['sym'])
                        ok[key] = ok.get(key, True) and is_world
    return ok


WORLD_ONLY = _world_only_uses()


def static_lead(lib, g):
    """How many of a live child's first children are pictures that never change."""
    L = LIB[lib]['syms']
    s_ = L.get(g.get('sym'), {})
    T = s_.get('T') or {}
    tr = g.get('tr', {})
    if not T or not set(T) <= LEAD_COMPS or not WORLD_ONLY.get((lib, g['sym'])) or tr.get('a', 1) < 1 or tr.get('v', 1) == 0:
        return 0
    frames = s_.get('frames') or []
    if not frames:
        return 0
    n = 0
    for i, c in enumerate(frames[0]):
        if not c.get('sym') or c['t'] not in ('b', 'm') or child_name(c) or not content_static(lib, c['sym']):
            break
        if c['tr'].get('v', 1) == 0 or any(i >= len(f) or f[i] != c for f in frames):
            break
        n += 1
    return n


def classify(lib, g, Mg, statics, kids, order_i, walls_only=False):
    """A child of a map (or of one of its regions) placed by Mg: baked scenery, or kept live.
    In rooms only invisible walls are taken out (the rest is small and drawn from the clip)."""
    L = LIB[lib]['syms']
    gs = L.get(g.get('sym'), {}) if g.get('sym') else {}
    T = gs.get('T') or {}
    dro = (T.get('drawOrderOverride') or {}).get('drawOrder')
    ground = 'tileBackground' in T or (dro is not None and dro <= -1000)
    bake = g['t'] == 'b' or ground or (g['t'] == 'm' and set(T) <= BAKE_COMPS and content_static(lib, g['sym']))
    if g['t'] in ('s', 'x') or g.get('tr', {}).get('nm'):
        bake = False
    if not bake:
        d = dict(g)
        d['tr'] = tr_of(Mg, g.get('tr', {}))
        kids.append(d)
        n = static_lead(lib, g) if not walls_only else 0
        if n:
            # the pictures under a door (the house itself) never change: they go into
            # the layer at the door's depth, and the door keeps the rest
            draws = []
            for c in gs['frames'][0][:n]:
                flatten_draws(lib, c['sym'], mat_mul(Mg, matrix(c.get('tr', {}))), c.get('tr', {}).get('a', 1), 0, draws)
            key = float(dro) if dro is not None else Mg[5]
            statics.append({'key': key, 'order': order_i, 'draws': draws, 'coll': None, 'sym': g.get('sym')})
            LEAD_BAKED[(lib, g['sym'])] = n
            return order_i + 1
        return order_i
    draws = []
    flatten_draws(lib, g['sym'], Mg, g.get('tr', {}).get('a', 1), 0, draws)
    if walls_only and (draws or 'collidable' not in T):
        d = dict(g)
        d['tr'] = tr_of(Mg, g.get('tr', {}))
        kids.append(d)
        return order_i
    coll = None
    if 'collidable' in T and 'boundable' in T:
        for bc in gs['frames'][0]:
            bs = L.get(bc.get('sym'), {}) if bc.get('sym') else {}
            if 'bounds' in (bs.get('T') or {}):
                r = sym_bounds(lib, bc['sym'])
                if r:
                    coll = aabb(mat_mul(Mg, matrix(bc.get('tr', {}))), r)
                break
    key = float(dro) if dro is not None else Mg[5]
    statics.append({'key': key, 'order': order_i, 'draws': draws, 'coll': coll, 'sym': g.get('sym')})
    return order_i + 1


# the rooms: each frame of the interior library's root shows one room's map
for c in [c for fr in LIB['interior']['syms']['mbb']['frames'] for c in fr if c.get('sym')]:
    if 'map' in (LIB['interior']['syms'][c['sym']].get('T') or {}) and ('interior', c['sym']) not in WORLDS:
        WORLDS.append(('interior', c['sym']))

for lib, mapsym in WORLDS:
    L = LIB[lib]['syms']
    m = L[mapsym]
    assert len(m['frames']) == 1
    statics, kids, regions = [], [], []
    order_i = 0
    for c in m['frames'][0]:
        rs = L.get(c.get('sym'), {}) if c.get('sym') else {}
        if 'region' not in (rs.get('T') or {}):
            order_i = classify(lib, c, matrix(c.get('tr', {})), statics, kids, order_i, lib == 'interior')
            continue
        Mr = matrix(c['tr'])
        nb = sym_bounds(lib, c['sym']) or rs.get('nb')     # Gj: getBounds, the union of its children
        regions.append((SYM_ID[(lib, c['sym'])], aabb(Mr, nb) if nb else (0, 0, 0, 0)))
        world_drop.add((lib, c['sym']))
        for g in rs['frames'][0]:
            order_i = classify(lib, g, mat_mul(Mr, matrix(g.get('tr', {}))), statics, kids, order_i)
    # the map keeps its live children
    for i, c in enumerate(kids):
        c['i'] = i
    m['frames'] = [kids]
    statics.sort(key=lambda st: (st['key'], st['order']))
    WORLD.append({'lib': lib, 'map': mapsym, 'statics': statics, 'regions': regions})
    for st in statics:
        for sp, M, a in st['draws']:
            if orientation(M) is None:
                world_warn['scaled or rotated scenery bitmap'] += 1
                if os.environ.get('WORLD_DEBUG'):
                    print('  scaled', st['sym'], sp, SPR[sp]['key'][3:], [round(v, 3) for v in M[:4]], round(a, 2))
            _, _, _, w, h = SPR[sp]['key']
            if w * h > STREAM_MIN:
                banded.add(sp)

# symbols only the regions show need no clip data: keep what is reachable otherwise
_keep = set()


def _reach(lib, k):
    st = [(lib, k)]
    while st:
        key = st.pop()
        if key in _keep or key in world_drop or key[1] not in LIB[key[0]]['syms']:
            continue
        _keep.add(key)
        s = LIB[key[0]]['syms'][key[1]]
        for fr in s.get('frames') or []:
            for c in fr:
                if c.get('sym'):
                    st.append((key[0], c['sym']))
        for comp in (s.get('T') or {}).values():
            if isinstance(comp, dict):
                for v in comp.values():
                    if isinstance(v, str) and v.startswith('<sym:'):
                        st.append((key[0], v[5:-1]))


_ha_refs = set(re.findall(r'\.ha\.([A-Za-z_$][\w$]*)', KITSUNE))
for lib, _ in LIBS:
    for k in LIB[lib]['syms']:
        if lib not in {w['lib'] for w in WORLD} or k in _ha_refs or any(k == w['map'] for w in WORLD):
            _reach(lib, k)
for w in WORLD:
    for k in LIB[w['lib']]['syms']:
        if (w['lib'], k) not in _keep:
            world_drop.add((w['lib'], k))
# a banded image must not also be drawn as a normal sprite
for (lib, k) in _keep:
    if (lib, k) in sym_sprite and sym_sprite[(lib, k)] in banded:
        world_warn['banded image also drawn as a sprite'] += 1
        banded.discard(sym_sprite[(lib, k)])
print('worlds: %s, %d symbols without clip data, %d banded images, warnings %s' % (
    ', '.join('%s %d statics %d live' % (w['map'], len(w['statics']), len(LIB[w['lib']]['syms'][w['map']]['frames'][0])) for w in WORLD),
    len(world_drop), len(banded), dict(world_warn)))


# which symbols (and so sprites) belong together: sprites reached from the same
# clip go in the same bank, in timeline order
order = []
seen_spr = set()


def walk_sprites(lib, k, depth=0, seen=None):
    if seen is None:
        seen = set()
    if (lib, k) in seen or depth > 12:
        return
    seen.add((lib, k))
    s = LIB[lib]['syms'][k]
    if (lib, k) in sym_sprite:
        sp = sym_sprite[(lib, k)]
        if sp not in seen_spr:
            seen_spr.add(sp)
            order.append(sp)
        return
    if s.get('t') == 'm':
        for fr in s['frames']:
            for c in fr:
                if c.get('sym'):
                    walk_sprites(lib, c['sym'], depth + 1, seen)


for lib, _ in LIBS:
    for k, s in LIB[lib]['syms'].items():
        if s.get('t') == 'm' and (lib, k) not in world_drop:
            walk_sprites(lib, k)
# then the scenery's images, in their draw order (neighbours share banks)
for w in WORLD:
    for st in w['statics']:
        for sp, _, _ in st['draws']:
            if sp not in seen_spr:
                seen_spr.add(sp)
                order.append(sp)
for (lib, k), sp in sym_sprite.items():
    if sp not in seen_spr and (lib, k) not in world_drop:
        seen_spr.add(sp)
        order.append(sp)


def sprite_pixels(sp):
    sh, x, y, w, h = SPR[sp]['key']
    return sheet_px[sh][y:y + h, x:x + w]


# classify, bank and compress
banks = []            # (raw bytes)
spr_rec = [None] * len(SPR)
streams = []
cur, cur_list = bytearray(), []


def flush():
    global cur, cur_list
    if cur:
        banks.append(bytes(cur))
        for sp, off in cur_list:
            spr_rec[sp] = ('bank', len(banks) - 1, off)
    cur, cur_list = bytearray(), []


for sp in order:
    sh, x, y, w, h = SPR[sp]['key']
    px = sprite_pixels(sp)
    if sp in banded:
        continue
    if w * h > STREAM_MIN:
        streams.append(sp)
        continue
    raw = px.tobytes()
    if len(cur) + len(raw) > BANK:
        flush()
    cur_list.append((sp, len(cur)))
    cur += raw
flush()

# ------------------------------------------------------------------ transforms
MATS = OrderedDict()
MATS[(1.0, 0.0, 0.0, 1.0)] = 0


def mat_id(a, b, c, d):
    key = tuple(round(v, 5) + 0.0 for v in (a, b, c, d))
    if key not in MATS:
        MATS[key] = len(MATS)
    return MATS[key]


# ------------------------------------------------------------------ components
COMPS = OrderedDict()
FIELDS = OrderedDict()
T_INT, T_FLOAT, T_BOOL, T_STR, T_VEC, T_SYM = 1, 2, 3, 4, 5, 6
# every component the engine knows (kitsune20.js registers them with G("name", ...)),
# so the C code can name any of them even if no symbol carries it
for _m in re.finditer(r'^G\("([^"]+)", ', KITSUNE, re.M):
    COMPS.setdefault(_m.group(1), len(COMPS))


def enc_T(T):
    out = bytearray()
    out.append(len(T))
    for comp, data in T.items():
        cid = COMPS.setdefault(comp, len(COMPS))
        if not isinstance(data, dict):
            data = {}
        out += bytes([cid, len(data)])
        for f, v in data.items():
            fid = FIELDS.setdefault(f, len(FIELDS))
            if isinstance(v, bool):
                out += struct.pack('<BBB', fid, T_BOOL, 1 if v else 0)
            elif isinstance(v, int) and -2 ** 31 <= v < 2 ** 31:
                out += struct.pack('<BBi', fid, T_INT, v)
            elif isinstance(v, int):
                out += struct.pack('<BBf', fid, T_FLOAT, float(v))
            elif isinstance(v, float):
                out += struct.pack('<BBf', fid, T_FLOAT, v)
            elif isinstance(v, str) and v.startswith('<sym:'):
                out += struct.pack('<BBH', fid, T_SYM, SYM_ID[(CUR_LIB[0], v[5:-1])])
            elif isinstance(v, str):
                out += struct.pack('<BBH', fid, T_STR, sid(v))
            elif isinstance(v, dict) and 'x' in v and 'y' in v:
                out += struct.pack('<BBff', fid, T_VEC, float(v['x']), float(v['y']))
            else:
                raise SystemExit('unsupported component value %r' % (v,))
    return bytes(out)


# ------------------------------------------------------------------ clips
SYM_BITMAP, SYM_CLIP, SYM_SHAPE, SYM_CONT, SYM_NONE = 1, 2, 3, 4, 0
MODES = {'independent': 0, 'single': 1, 'synched': 2}
# keyframe flags
K_ABSENT, K_MAT, K_ALPHA, K_REG, K_CLIP, K_NAME, K_HIDDEN, K_KIND = 1, 2, 4, 8, 16, 32, 64, 128
CK_SYM, CK_SHAPE, CK_TEXT = 0, 1, 2
anon_shapes = {}




SLOT_CONFLICTS = []


def slot_order(slots, frames):
    """The children's stacking order over all frames: each frame's own order
    (a child that shows up later, such as a button's focus background, still
    goes under the ones it is drawn under), as a topological sort; where two
    frames disagree the first one wins. Ties: where a child first stands."""
    first = {i: min((v[0], fi) for fi, v in enumerate(slots[i]) if v is not None) for i in slots}
    succ = {i: set() for i in slots}

    def reaches(a, b):
        todo, seen = [a], set()
        while todo:
            x = todo.pop()
            if x == b:
                return True
            if x not in seen:
                seen.add(x)
                todo.extend(succ[x])
        return False

    for fr in frames:
        order = [c['i'] for c in fr]
        for a, b in zip(order, order[1:]):
            if a == b or b in succ[a]:
                continue
            if reaches(b, a):
                SLOT_CONFLICTS.append((a, b))
                continue
            succ[a].add(b)
    indeg = {i: 0 for i in slots}
    for a in slots:
        for b in succ[a]:
            indeg[b] += 1
    ready = sorted((first[i], i) for i in slots if not indeg[i])
    out = []
    while ready:
        _, x = ready.pop(0)
        out.append(x)
        for b in succ[x]:
            indeg[b] -= 1
            if not indeg[b]:
                ready.append((first[b], b))
                ready.sort()
    return out


def child_state(lib, c):
    """What a child slot shows at a frame, as a hashable tuple."""
    tr = c['tr']
    a, b, cc, d, tx, ty = matrix(tr)
    kind, ref = CK_SYM, 0xFFFF
    if c.get('sym') and (lib, c['sym']) in SYM_ID:
        ref = SYM_ID[(lib, c['sym'])]
    elif c['t'] == 's':
        kind = CK_SHAPE
        ref = enc_shape(c.get('g') or [])
    elif c['t'] == 'x':
        kind = CK_TEXT
        ref = enc_text(c)
    alpha = tr.get('a', 1)
    clip = None
    if c['t'] == 'm':
        clip = (MODES.get(c.get('mode'), 0), c.get('sp') or 0, 1 if c.get('loop', True) else 0)
    # entities (clips with components) and named children keep CreateJS's x, y and
    # registration point, for the game code; for the rest they fold into the translation
    keep = bool(child_name(c)) or (c.get('sym') and LIB[lib]['syms'].get(c['sym'], {}).get('T') is not None)
    if keep:
        x, y, rx, ry = tr.get('x', 0), tr.get('y', 0), tr.get('rx', 0), tr.get('ry', 0)
    else:
        x, y, rx, ry = tx, ty, 0, 0
    return (kind, ref, round(a, 5), round(b, 5), round(cc, 5), round(d, 5), round(x * 4), round(y * 4),
            int(round(max(0, min(1, alpha)) * 255)), clip, child_name(c), tr.get('v', 1) == 0,
            round(rx * 4), round(ry * 4))


PAYLOADS = OrderedDict()     # shapes and texts placed directly in clips


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def enc_key(start, st):
    if st is None:
        return varint(start) + bytes([K_ABSENT])
    kind, ref, a, b, c, d, tx4, ty4, alpha, clip, nm, hidden, rx4, ry4 = st
    flags = 0
    m = mat_id(a, b, c, d)
    if m:
        flags |= K_MAT
    if alpha != 255:
        flags |= K_ALPHA
    assert -32768 <= tx4 < 32768 and -32768 <= ty4 < 32768 and -32768 <= rx4 < 32768 and -32768 <= ry4 < 32768
    if rx4 or ry4:
        flags |= K_REG
    if clip and clip != (0, 0, 1):
        flags |= K_CLIP
    if nm:
        flags |= K_NAME
    if hidden:
        flags |= K_HIDDEN
    if kind != CK_SYM:
        flags |= K_KIND
    out = bytearray(varint(start))
    out.append(flags)
    if kind != CK_SYM:
        out.append(kind)
        out += struct.pack('<H', PAYLOADS.setdefault(ref, len(PAYLOADS)))
    else:
        out += struct.pack('<H', ref)
    out += struct.pack('<hh', tx4, ty4)
    if m:
        out += struct.pack('<H', m)
    if flags & K_REG:
        out += struct.pack('<hh', rx4, ry4)
    if alpha != 255:
        out.append(alpha)
    if flags & K_CLIP:
        out += struct.pack('<BHB', clip[0], clip[1], clip[2])
    if nm:
        out += struct.pack('<H', sid(nm))
    return bytes(out)


ACT_STOP, ACT_DISPATCH_PARENT, ACT_DISPATCH, ACT_PLAY = 1, 2, 3, 4


def enc_actions(acts):
    out = []
    for t, src in acts:
        body = ' '.join(src.split())
        body = re.sub(r'this\.T = \{.*?\};?', '', body)
        for m in re.finditer(r'this\.parent\.dispatchEvent\("([^"]*)"\)|this\.dispatchEvent\("([^"]*)"\)|this\.stop\(\)|this\.play\(\)', body):
            if m.group(1) is not None:
                out.append((t, ACT_DISPATCH_PARENT, sid(m.group(1))))
            elif m.group(2) is not None:
                out.append((t, ACT_DISPATCH, sid(m.group(2))))
            elif 'stop' in m.group(0):
                out.append((t, ACT_STOP, 0))
            else:
                out.append((t, ACT_PLAY, 0))
    return out


STATIC = {}


def is_static(lib, k, depth=0):
    """A clip that never changes and that code never touches: drawn from the data, no node."""
    key = (lib, k)
    if key in STATIC:
        return STATIC[key]
    s = LIB[lib]['syms'].get(k, {})
    t = s.get('t')
    if t in ('b', 's'):
        return True
    if t != 'm' or depth > 20:
        return False
    STATIC[key] = False   # cycles: not static
    acts = enc_actions(s.get('acts') or [])
    # many clips have extra frames but stop on the first one, with nothing to jump to
    stopped = acts == [(0, ACT_STOP, 0)] and not s.get('labels')
    ok = (len(s['frames']) == 1 and not acts or stopped) and s.get('T') is None
    if ok:
        for c in s['frames'][0]:
            if child_name(c) or c['t'] == 'x' or c.get('mask') or c.get('hit'):
                ok = False
                break
            if c['t'] == 'm' and not (c.get('sym') and is_static(lib, c['sym'], depth + 1)):
                ok = False
                break
    STATIC[key] = ok
    return ok


sym_table = []
clip_dedupe = {}
T_TABLE = OrderedDict()
CUR_LIB = ['']
for (lib, k) in SYMS:
    s = LIB[lib]['syms'][k]
    CUR_LIB[0] = lib
    t = s.get('t')
    if (lib, k) in world_drop:
        sym_table.append((SYM_NONE, 0))
    elif t == 'b':
        sym_table.append((SYM_BITMAP, sym_sprite.get((lib, k), 0xFFFF)))
    elif t == 's':
        sym_table.append((SYM_SHAPE, PAYLOADS.setdefault(enc_shape(s.get('g') or []), len(PAYLOADS))))
    elif t == 'm':
        frames = s['frames']
        if (lib, k) in LEAD_BAKED:
            frames = [f[LEAD_BAKED[(lib, k)]:] for f in frames]
        nf = len(frames)
        slots = defaultdict(lambda: [None] * nf)
        for fi, fr in enumerate(frames):
            for depth, c in enumerate(fr):
                slots[c['i']][fi] = (depth, child_state(lib, c))
        slot_ids = slot_order(slots, frames)
        body = bytearray()
        for i in slot_ids:
            keys, prev = [], 'x'
            for fi in range(nf):
                v = slots[i][fi]
                st = v[1] if v else None
                if st != prev:
                    keys.append(enc_key(fi, st))
                    prev = st
            body += varint(len(keys)) + b''.join(keys)
        labels = sorted((pos, sid(lab)) for lab, pos in (s.get('labels') or []))
        acts = enc_actions(s.get('acts') or [])
        T = s.get('T')
        ti = T_TABLE.setdefault(enc_T(T), len(T_TABLE)) if T else 0xFFFF
        nb = s.get('nb') or [0, 0, 0, 0]
        hdr = struct.pack('<HHBBH', nf, len(slot_ids), len(labels), len(acts), ti)
        hdr += struct.pack('<hhhh', *(max(-32768, min(32767, int(round(v)))) for v in nb))
        hdr += b''.join(struct.pack('<HH', p_, l_) for p_, l_ in labels)
        hdr += b''.join(struct.pack('<HBH', t_, kd, a_) for t_, kd, a_ in acts)
        data = bytes(hdr + body)
        if data not in clip_dedupe:
            clip_dedupe[data] = blob.add(data, 2, cat='clips')
        sym_table.append((SYM_CLIP, clip_dedupe[data]))
    elif t == 'c':
        sym_table.append((SYM_CONT, 0))
    else:
        sym_table.append((SYM_NONE, 0))

# ------------------------------------------------------------------ quest conditions
VARS = OrderedDict()


def var_id(name):
    return VARS.setdefault(name, len(VARS))


E_VAR, E_STR, E_NUM, E_TRUE, E_FALSE, E_EQ, E_NE, E_LT, E_GT, E_LE, E_GE, E_AND, E_OR, E_NOT, E_ADD, E_END = range(16)


def compile_expr(src):
    """Kitsune's conditions (expr-eval after &&->and, ||->or, !$X->not $X, null->false) to RPN."""
    s = src.strip()
    if not s:
        return bytes([E_TRUE, E_END])
    toks = re.findall(r"\$[A-Za-z_0-9]+|'[^']*'|\"[^\"]*\"|\d+(?:\.\d+)?|==|!=|<=|>=|&&|\|\||[()<>!+]|[A-Za-z_]+", s)
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    out = bytearray()

    def primary():
        t = take()
        if t == '(':
            orx()
            assert take() == ')'
        elif t in ('!', 'not'):
            primary()
            out.append(E_NOT)
        elif t.startswith('$'):
            out.extend(struct.pack('<BH', E_VAR, var_id(t[1:])))
        elif t[0] in '\'"':
            out.extend(struct.pack('<BH', E_STR, sid(t[1:-1])))
        elif re.match(r'\d', t):
            out.extend(struct.pack('<Bf', E_NUM, float(t)))
        elif t == 'true':
            out.append(E_TRUE)
        elif t in ('false', 'null'):
            out.append(E_FALSE)
        else:
            raise SystemExit('bad token %r in %r' % (t, src))

    def add():
        primary()
        while peek() == '+':
            take()
            primary()
            out.append(E_ADD)

    def cmp():
        add()
        ops = {'==': E_EQ, '!=': E_NE, '<': E_LT, '>': E_GT, '<=': E_LE, '>=': E_GE}
        while peek() in ops:
            op = ops[take()]
            add()
            out.append(op)

    def andx():
        cmp()
        while peek() in ('&&', 'and'):
            take()
            cmp()
            out.append(E_AND)

    def orx():
        andx()
        while peek() in ('||', 'or'):
            take()
            andx()
            out.append(E_OR)

    orx()
    assert pos == len(toks), (src, toks)
    out.append(E_END)
    return bytes(out)


EXPRS = OrderedDict()


def expr_id(src):
    if src not in EXPRS:
        EXPRS[src] = len(EXPRS)
    return EXPRS[src]


# every condition string found in components gets compiled; the component keeps it as a string
for lib, _ in LIBS:
    for s in LIB[lib]['syms'].values():
        for comp, data in (s.get('T') or {}).items():
            if isinstance(data, dict):
                for f, v in data.items():
                    if f.startswith('condition') and isinstance(v, str):
                        expr_id(v)

# ------------------------------------------------------------------ dialogue
def storage_value(v):
    """Encodes a JS value stored by the game: type byte + payload."""
    if v is None:
        return struct.pack('<BI', 0, 0)
    if isinstance(v, bool):
        return struct.pack('<BI', 1, 1 if v else 0)
    if isinstance(v, (int, float)):
        return struct.pack('<Bf', 2, float(v))
    return struct.pack('<BI', 3, sid(v))


# the dialogue's lines are kept apart, compressed (see dtext below): only the
# dialogue box reads them, one at a time
DTEXT = OrderedDict()
DLG_KEYS = set()


def dtext_id(t):
    if t is None:
        return 0xFFFF
    return DTEXT.setdefault(t, len(DTEXT))


dialog_groups = []
for g in DIALOG:
    nodes = []
    for n in g:
        # texts come from the translations (npc + node name), else the tree's own
        base = n['V'] + n['nodeName']
        DLG_KEYS.add(base)
        n['text'] = MESSAGES.get(base, n.get('text'))
        opts = []
        for k, o in enumerate(n.get('U', [])):
            DLG_KEYS.add(base + 'opt' + str(k))
            opts.append((sid(o.get('next')), dtext_id(MESSAGES.get(base + 'opt' + str(k), o.get('text')) if o.get('text') else None)))
        tags = n.get('tags', {})
        avatar = (tags.get('W') or [''])[0]
        zd = tags.get('zd')
        setv = (var_id(zd[0]), storage_value(zd[1])) if zd else None
        nodes.append((sid(n['nodeName']), sid(n['V']), sid(avatar), dtext_id(n.get('text')), opts, setv))
    dialog_groups.append(nodes)

dlg = bytearray()
dlg_index = []
for nodes in dialog_groups:
    for (name, npc, avatar, text, opts, setv) in nodes:
        rec = struct.pack('<HHHHBB', name, npc, avatar, text, len(opts), 1 if setv else 0)
        for nx, tx in opts:
            rec += struct.pack('<HH', nx, tx)
        if setv:
            rec += struct.pack('<H', setv[0]) + setv[1]
        dlg_index.append(len(dlg))
        dlg += rec
        while len(dlg) % 2:
            dlg.append(0)
off_dlg = blob.add(bytes(dlg))
off_dlg_index = blob.add(b''.join(struct.pack('<I', off_dlg + o) for o in dlg_index))

def bpe(texts):
    """Byte-pair encoding: bytes the texts never use stand for frequent pairs."""
    data = [list(t.encode('utf-8')) for t in texts]
    used = set(b for t in data for b in t) | {0}
    pairs = {}
    for code in [b for b in range(1, 256) if b not in used]:
        cnt = Counter()
        for t in data:
            cnt.update(zip(t, t[1:]))
        if not cnt:
            break
        (a, b), n = cnt.most_common(1)[0]
        if n < 3:
            break
        pairs[code] = (a, b)
        for i, t in enumerate(data):
            out, j = [], 0
            while j < len(t):
                if j + 1 < len(t) and t[j] == a and t[j + 1] == b:
                    out.append(code)
                    j += 2
                else:
                    out.append(t[j])
                    j += 1
            data[i] = out
    return data, pairs


_dt, _pairs = bpe(list(DTEXT))
_tab = bytearray(512)
for c, (a, b) in _pairs.items():
    _tab[2 * c], _tab[2 * c + 1] = a, b
_codes = bytearray(32)
for c in _pairs:
    _codes[c >> 3] |= 1 << (c & 7)
_offs, _blob = [], bytearray()
for t in _dt:
    _offs.append(len(_blob))
    _blob += bytes(t)
_offs.append(len(_blob))
off_dtext = blob.add(struct.pack('<H', len(_dt)) + b'\0\0' + bytes(_codes) + bytes(_tab) +
                     b''.join(struct.pack('<I', o) for o in _offs) + bytes(_blob), cat='dtext')
print('dialogue texts: %d lines, %d KB -> %d KB with %d pairs' % (len(_dt), sum(len(t.encode()) for t in DTEXT) // 1024,
      len(_blob) // 1024, len(_pairs)))

# messages (translatable UI texts), sorted by key for binary search
_named = set(re.findall(r'"([A-Za-z0-9_]+)"', KITSUNE))
for _lib in LIB.values():
    for _s in _lib['syms'].values():
        for _c in (_s.get('T') or {}).values():
            if isinstance(_c, dict):
                _named.update(v for v in _c.values() if isinstance(v, str))
msg_keys = sorted(k for k in MESSAGES if k not in DLG_KEYS or k in _named)
off_msgs = blob.add(b''.join(struct.pack('<HH', sid(k), sid(MESSAGES[k])) for k in msg_keys))

# compiled expressions
exprs_bin = [compile_expr(src) for src in EXPRS]
expr_offs = [blob.add(e, 1) for e in exprs_bin]
off_exprs = blob.add(b''.join(struct.pack('<HI', sid(src), o) for src, o in zip(EXPRS, expr_offs)))

# ------------------------------------------------------------------ sprites & banks
bank_offs = []
for raw in banks:
    comp = lzma_raw(raw, len(raw))
    bank_offs.append((blob.add(comp, 1), len(comp), len(raw)))


def strips(px):
    """Where a stream's strips start: rows are added 8 at a time while the strip compresses to STRIP_BYTES."""
    h, cuts = px.shape[0], [0]
    while h > STRIP_MIN_H:
        y = cuts[-1] + 8
        while y < h and len(lzma_raw(px[cuts[-1]:y + 8].tobytes(), DICT)) <= STRIP_BYTES:
            y += 8
        if y >= h:
            break
        cuts.append(y)
    return cuts + [h]


stream_off = {}       # per stream, its strips: (offset, compressed size, raw size)
for sp in streams:
    px = sprite_pixels(sp)
    cuts = strips(px)
    stream_off[sp] = []
    for y0, y1 in zip(cuts, cuts[1:]):
        comp = lzma_raw(px[y0:y1].tobytes(), DICT)
        stream_off[sp].append((blob.add(comp, 1), len(comp), (y1 - y0) * px.shape[1]))
# scenery images: columns of BAND_W pixels, each its own stream, so that painting
# a strip of the world decodes only the columns it needs
band_off = {}
for sp in sorted(banded):
    px = sprite_pixels(sp)
    h, w = px.shape
    recs = []
    for x in range(0, w, BAND_W):
        comp = lzma_raw(px[:, x:x + BAND_W].tobytes(), DICT)
        recs.append((blob.add(comp, 1, cat='bands'), len(comp)))
    band_off[sp] = blob.add(struct.pack('<H', len(recs)) + b''.join(struct.pack('<II', o, n) for o, n in recs))

def rle_size(sp):
    """Bytes of the cache's run-length form (spr.c: rle_row), exactly."""
    sh, x, y, w, h = SPR[sp]['key']
    px = sprite_pixels(sp)
    al = sheet_alpha[sh]
    n = 4 + 2 * h
    for row in px:
        row = row.tolist()
        op = [al[c] > 0 for c in row]
        n += 2
        xx = 0
        while xx < w:
            skip = 0
            while xx < w and not op[xx]:
                xx += 1
                skip += 1
            if xx >= w:
                break
            n += 2 * ((skip - 1) // 255) if skip > 255 else 0
            run = 1
            while xx + run < w and run < 128 and row[xx + run] == row[xx]:
                run += 1
            if run >= 3:
                n += 3
                xx += run
                continue
            ln = 0
            while xx + ln < w and ln < 127 and op[xx + ln] and not (
                    xx + ln + 2 < w and row[xx + ln] == row[xx + ln + 1] and row[xx + ln] == row[xx + ln + 2]):
                ln += 1
            ln = max(ln, 1)
            n += 2 + ln
            xx += ln
    return n


spr_bin = bytearray()
for sp, rec in enumerate(SPR):
    sh, x, y, w, h = rec['key']
    if sp in stream_off:
        o, cl, rl = stream_off[sp][0]
        spr_bin += struct.pack('<HHBBHII', w, h, sh, 1, 0, o, rle_size(sp))
    elif sp in band_off:
        spr_bin += struct.pack('<HHBBHII', w, h, sh, 2, 0, band_off[sp], 0)
    elif spr_rec[sp]:
        _, bank, off = spr_rec[sp]
        spr_bin += struct.pack('<HHBBHII', w, h, sh, 0, bank, off, rle_size(sp))
    else:   # never shown
        spr_bin += struct.pack('<HHBBHII', w, h, sh, 3, 0, 0, 0)
off_spr = blob.add(bytes(spr_bin))
off_banks = blob.add(b''.join(struct.pack('<III', *b) for b in bank_offs))
# the sprites of each bank, in the order they come out of the decoder
bank_members = defaultdict(list)
for sp, rec in enumerate(spr_rec):
    if rec:
        bank_members[rec[1]].append((rec[2], sp))
bm_offs = []
for b in range(len(banks)):
    m = sorted(bank_members[b])
    bm_offs.append(blob.add(struct.pack('<H', len(m)) + b''.join(struct.pack('<H', sp) for _, sp in m), 2))
off_bankspr = blob.add(b''.join(struct.pack('<I', o) for o in bm_offs))
# the stream table: a record per strip, those of a stream in a row
stream_recs = [(sp, *st) for sp in streams for st in stream_off[sp]]
off_streams = blob.add(b''.join(struct.pack('<HIII', *r) for r in stream_recs))

pal_bin = bytearray()
for i in range(len(SHEETS)):
    for (r, g, b) in pal_rgb[i]:
        pal_bin += struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    pal_bin += bytes(sheet_alpha[i])
off_pal = blob.add(bytes(pal_bin))

# Patterns drawn over the island straight from here, with their own colours
# (u16 rgb565[16], u8 alpha 0..32 [16], index 0 clear):
# - the rain (bca): its two looks (Yi shows u_a, then v_a), 4 bits a pixel;
# - the petals after the ending (mAa): the 20 looks of a petal (Ue), each as
#   its few pixels (x, y, colour), where Ue shows it.
# Block: u16 sets, u16 0, u32 set offsets; a set: u16 kind (0 pixels, 1 points),
# u16 looks, u16 w, u16 h, colours, then the pixels, or u32 look offsets and per
# look u16 points, then (x, y, colour) bytes.
def pattern_colours(sh, vals):
    cols = []
    for v in vals:
        if sheet_alpha[sh][v] and v not in cols:
            cols.append(v)
    assert len(cols) <= 15, cols
    pal, al = [0] * 16, [0] * 16
    for i, v in enumerate(cols):
        r, g, b = pal_rgb[sh][v]
        pal[i + 1] = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
        al[i + 1] = (int(sheet_alpha[sh][v]) * 32 + 127) // 255
    return cols, struct.pack('<16H', *pal) + bytes(al)


def tiles4(lib, names):
    looks = [LIB[lib]['syms'][n]['sheet'] for n in names]
    sh, _, _, w, h = looks[0]
    assert all(l[0] == sh and l[3:] == (w, h) for l in looks) and w % 2 == 0
    cols, colours = pattern_colours(sh, [v for _, x, y, _, _ in looks for v in sheet_px[sh][y:y + h, x:x + w].flatten().tolist()])
    px = bytearray()
    for _, x, y, _, _ in looks:
        for row in sheet_px[sh][y:y + h, x:x + w]:
            nib = [cols.index(v) + 1 if sheet_alpha[sh][v] else 0 for v in row.tolist()]
            px += bytes(nib[i] | nib[i + 1] << 4 for i in range(0, w, 2))
    return struct.pack('<HHHH', 0, len(looks), w, h) + colours + bytes(px)


def points(lib, clip):
    """Each frame of `clip` (one bitmap child) as its visible pixels, where it stands."""
    L = LIB[lib]['syms']
    frames = L[clip]['frames']
    step = 3
    shots = []
    for f in range(0, len(frames), step):
        c = frames[f][0]
        sh, x, y, w, h = L[c['sym']]['sheet']
        dx, dy = int(round(c['tr'].get('x', 0))), int(round(c['tr'].get('y', 0)))
        shots.append((sh, x, y, w, h, dx, dy))
    sh = shots[0][0]
    cols, colours = pattern_colours(sh, [v for s_, x, y, w, h, _, _ in shots for v in sheet_px[s_][y:y + h, x:x + w].flatten().tolist()])
    bodies = []
    for s_, x, y, w, h, dx, dy in shots:
        pts = [(dx + i, dy + j, cols.index(v) + 1) for j, row in enumerate(sheet_px[s_][y:y + h, x:x + w].tolist())
               for i, v in enumerate(row) if sheet_alpha[s_][v]]
        assert all(0 <= a < 256 and 0 <= b < 256 for a, b, _ in pts)
        bodies.append(struct.pack('<H', len(pts)) + b''.join(bytes(p) for p in pts))
    mw = max(dx + w for _, _, _, w, h, dx, dy in shots)
    mh = max(dy + h for _, _, _, w, h, dx, dy in shots)
    head = struct.pack('<HHHH', 1, len(bodies), mw, mh) + colours
    offs, at = [], len(head) + 4 * len(bodies)
    for b_ in bodies:
        offs.append(at)
        at += len(b_)
    return head + b''.join(struct.pack('<I', o) for o in offs) + b''.join(bodies)


def rle_bytes(sp):
    """The cache's run-length form of a sprite (spr.c: rle_row), to be read from here."""
    sh, x, y, w, h = SPR[sp]['key']
    al = sheet_alpha[sh]
    rows = []
    for row in sprite_pixels(sp).tolist():
        out, n, xx = bytearray(), 0, 0
        while xx < w:
            skip = 0
            while xx < w and not al[row[xx]]:
                xx += 1
                skip += 1
            if xx >= w:
                break
            while skip > 255:
                out += bytes([255, 0])
                skip -= 255
                n += 1
            run = 1
            while xx + run < w and run < 128 and row[xx + run] == row[xx]:
                run += 1
            if run >= 3:
                out += bytes([skip, 0x80 | (run - 1), row[xx]])
                xx += run
            else:
                ln = 0
                while xx + ln < w and ln < 127 and al[row[xx + ln]] and not (
                        xx + ln + 2 < w and row[xx + ln] == row[xx + ln + 1] and row[xx + ln] == row[xx + ln + 2]):
                    ln += 1
                ln = max(ln, 1)
                out += bytes([skip, ln]) + bytes(row[xx:xx + ln])
                xx += ln
            n += 1
        rows.append(struct.pack('<H', n) + out)
    offs, at = [], 4 + 2 * h
    for r in rows:
        offs.append(at)
        at += len(r)
    data = struct.pack('<HH', w, h) + b''.join(struct.pack('<H', o) for o in offs) + b''.join(rows)
    assert len(data) == rle_size(sp), (len(data), rle_size(sp))
    return data


# the ending's glow (nAa): a whole screen, but little once in runs
_glow = sym_sprite[('overworld', 'nAa')]
_sets = [tiles4('overworld', ['u_a', 'v_a']), points('overworld', 'Ue'), struct.pack('<HH', 2, _glow) + rle_bytes(_glow)]
_blk = bytearray(struct.pack('<HH', len(_sets), 0))
_at = 4 + 4 * len(_sets)
for _s in _sets:
    _at = (_at + 3) & ~3
    _blk += struct.pack('<I', _at)
    _at += len(_s)
for _s in _sets:
    while len(_blk) % 4:
        _blk += b'\0'
    _blk += _s
off_tiles = blob.add(bytes(_blk), 4)
print('patterns: %d bytes' % len(_blk))

pay_offs = [blob.add(p_, 2, cat='payload') for p_ in PAYLOADS]
off_payloads = blob.add(b''.join(struct.pack('<I', o) for o in pay_offs))
t_offs = [blob.add(t_, 1, cat='T') for t_ in T_TABLE]
off_ttab = blob.add(b''.join(struct.pack('<I', o) for o in t_offs))
off_mats = blob.add(b''.join(struct.pack('<ffff', *m) for m in MATS))
LIB_INDEX = {n: i for i, (n, _) in enumerate(LIBS)}
off_syms = blob.add(b''.join(struct.pack('<BBHI', t, LIB_INDEX[lib], 1 if (t == SYM_CLIP and is_static(lib, k)) else 0, v)
                             for (t, v), (lib, k) in zip(sym_table, SYMS)))

# worlds: statics sorted in draw order, a grid index over them, the regions
def lut444(sheet):
    """RGB444 -> nearest opaque colour of the sheet, to blend into the 8-bit layer."""
    pal = np.array(pal_rgb[sheet], np.float32)
    ok = sheet_alpha[sheet] == 255
    out = bytearray(4096)
    for i in range(4096):
        c = np.array([((i >> 8) & 15) * 17, ((i >> 4) & 15) * 17, (i & 15) * 17], np.float32)
        d = ((pal - c) ** 2).sum(1)
        d[~ok] = 1e12
        out[i] = int(np.argmin(d))
    return bytes(out)


world_offs = []
LUT_OFF = {}


def grid_of(boxes, gx0, gy0, gw, gh):
    cells = [[] for _ in range(gw * gh)]
    for i, b in enumerate(boxes):
        for cy in range((b[1] - gy0) // WORLD_CELL, (b[3] - 1 - gy0) // WORLD_CELL + 1):
            for cx in range((b[0] - gx0) // WORLD_CELL, (b[2] - 1 - gx0) // WORLD_CELL + 1):
                cells[cy * gw + cx].append(i)
    starts, lst = [], []
    for c in cells:
        starts.append(len(lst))
        lst += c
    starts.append(len(lst))
    assert len(lst) < 65536
    return (blob.add(b''.join(struct.pack('<H', v) for v in starts), 2, cat='world'),
            blob.add(b''.join(struct.pack('<H', v) for v in lst), 2, cat='world'), len(lst))


for w in WORLD:
    recs, boxes, walls = [], [], []
    sheets = Counter()
    for st in w['statics']:
        draws = []
        coll = st['coll']
        cb = (int(math.floor(coll[0])), int(math.floor(coll[1])), int(math.ceil(coll[2])), int(math.ceil(coll[3]))) if coll else None
        for sp, M, a in st['draws']:
            fl = orientation(M)
            if fl is None:
                continue
            sh, _, _, sw, shh = SPR[sp]['key']
            sheets[sh] += 1
            x0_, y0_, _, _ = aabb(M, (0, 0, sw, shh))
            dw, dh = (shh, sw) if fl & 4 else (sw, shh)
            draws.append((sp, int(math.floor(x0_ + 0.5)), int(math.floor(y0_ + 0.5)), dw, dh, fl, int(round(min(1, a) * 255))))
        if not draws:
            if cb:
                walls.append(cb)
            continue
        bx0, by0 = min(d[1] for d in draws), min(d[2] for d in draws)
        bx1, by1 = max(d[1] + d[3] for d in draws), max(d[2] + d[4] for d in draws)
        if cb:
            bx0, by0, bx1, by1 = min(bx0, cb[0]), min(by0, cb[1]), max(bx1, cb[2]), max(by1, cb[3])
        rec = struct.pack('<fhhHHBB', st['key'], bx0, by0, bx1 - bx0, by1 - by0, len(draws), 1 if cb else 0)
        if cb:
            rec += struct.pack('<hhhh', *cb)
        for sp, x, y, dw, dh, fl, al in draws:
            dx, dy = x - bx0, y - by0
            wide = dx > 255 or dy > 255
            rec += struct.pack('<HB', sp, fl | (8 if al != 255 else 0) | (16 if wide else 0))
            rec += struct.pack('<HH', dx, dy) if wide else struct.pack('<BB', dx, dy)
            if al != 255:
                rec += bytes([al])
        while len(rec) % 4:
            rec += b'\0'
        recs.append(rec)
        boxes.append((bx0, by0, bx1, by1))
    allb = boxes + walls
    if allb:
        gx0 = min(b[0] for b in allb) // WORLD_CELL * WORLD_CELL
        gy0 = min(b[1] for b in allb) // WORLD_CELL * WORLD_CELL
        gw = (max(b[2] for b in allb) - gx0) // WORLD_CELL + 1
        gh = (max(b[3] for b in allb) - gy0) // WORLD_CELL + 1
    else:
        gx0 = gy0 = 0
        gw = gh = 1
    off_cells, off_list, nrefs = grid_of(boxes, gx0, gy0, gw, gh)
    off_wcells, off_wlist, nwrefs = grid_of(walls, gx0, gy0, gw, gh)
    base = blob.add(b'', 4, cat='world')
    rel = []
    for r in recs:
        rel.append((blob.add(r, 4, cat='world') - base) // 4)
    assert not rel or max(rel) < 65536
    off_stoffs = blob.add(b''.join(struct.pack('<H', v) for v in rel), 2, cat='world')
    off_walls = blob.add(b''.join(struct.pack('<hhhh', *b) for b in walls), 2, cat='world')
    off_reg = blob.add(b''.join(struct.pack('<Hhhhh', sym, *(int(round(v)) for v in r)) for sym, r in w['regions']), 2, cat='world')
    sheet = sheets.most_common(1)[0][0] if sheets else SHEETS.index('overworld-sprite.png')
    if len(sheets) > 1:
        print('world %s: images from sheets %s' % (w['map'], dict(sheets)))
    if sheet not in LUT_OFF:
        LUT_OFF[sheet] = blob.add(lut444(sheet), cat='world')
    world_offs.append(blob.add(struct.pack('<HHHHHHhhHHIIIIIIIII', SYM_ID[(w['lib'], w['map'])], len(recs), len(walls), len(w['regions']),
                                           sheet, WORLD_CELL, gx0, gy0, gw, gh, off_cells, off_list, off_wcells, off_wlist,
                                           off_stoffs, base, off_walls, off_reg, LUT_OFF[sheet])))
    if w['lib'] == 'overworld' or os.environ.get('WORLD_DEBUG'):
        print('world %s: %d statics, %d walls, grid %dx%d, %d+%d refs, %d KB' % (w['map'], len(recs), len(walls), gw, gh, nrefs, nwrefs,
              (sum(len(r) for r in recs) + 2 * (nrefs + nwrefs + 2 * gw * gh + len(recs)) + 8 * len(walls)) // 1024))
off_worlds = blob.add(struct.pack('<I', len(world_offs)) + b''.join(struct.pack('<I', o) for o in world_offs))

# the saved values' names (store.c keeps a name as its string id)
_SPORTS = ['archery', 'climbing', 'marathon', 'pingpong', 'rugby', 'skate', 'swim']
for _k in ['PLAYER_LOC', 'PLAYER_POS_X', 'PLAYER_POS_Y', 'PLAYER_TEAM', 'SUBMITTED_SCORES', 'TUTORIAL_DONE', 'TUTORIAL_BEGIN', 'FIRST_PLACE']:
    sid(_k)
for _r in re.findall(r'"?([a-z]+(?::[a-z0-9]+)?)"?: \{type: "(?:points|time)"', KITSUNE):
    sid(_r + '_score')
    sid(_r + '_rating')
for _v in ['intro', 'outro'] + [x + 'intro' for x in _SPORTS] + [x + 'outro' for x in _SPORTS]:
    sid(_v + '_VIDEO_SEEN')
for _x in _SPORTS:
    sid(_x + '_TUTORIAL_SEEN')
for _v in VARS:
    sid(_v)

# strings last (everything above may have added some)
str_offs, str_data = [], bytearray()
for s in strings:
    str_offs.append(len(str_data))
    str_data += s.encode('utf-8') + b'\0'
off_strtab = blob.add(b''.join(struct.pack('<I', o) for o in str_offs))
# string ids in byte order, for str_find's binary search
_sl = list(strings)
off_strsort = blob.add(b''.join(struct.pack('<H', i) for i in sorted(range(len(_sl)), key=lambda i: _sl[i].encode('utf-8'))), 2)
off_strdata = blob.add(bytes(str_data), 1)

HEADER = [
    ('magic', 0x31304943), ('nsheets', len(SHEETS)), ('pal', off_pal), ('nsprites', len(SPR)), ('sprites', off_spr),
    ('nbanks', len(banks)), ('banks', off_banks), ('nstreams', len(stream_recs)), ('streams', off_streams),
    ('nsyms', len(SYMS)), ('syms', off_syms), ('nmats', len(MATS)), ('mats', off_mats),
    ('nstrings', len(strings)), ('strtab', off_strtab), ('strdata', off_strdata),
    ('nexprs', len(EXPRS)), ('exprs', off_exprs), ('ndlg', len(dlg_index)), ('dlg', off_dlg_index),
    ('nmsgs', len(msg_keys)), ('msgs', off_msgs), ('nvars', len(VARS)), ('payloads', off_payloads), ('ttab', off_ttab), ('bankspr', off_bankspr), ('worlds', off_worlds), ('dtext', off_dtext), ('strsort', off_strsort), ('tiles', off_tiles)]
assert len(HEADER) == HEADER_WORDS
final = bytearray(blob.b)
final[:4 * HEADER_WORDS] = b''.join(struct.pack('<I', v) for _, v in HEADER)
open(os.path.join(ROOT, 'src/data.bin'), 'wb').write(final)

# ------------------------------------------------------------------ header for C
h = ['/* Generated by tools/pack.py. Do not edit. */', '#ifndef CI_DATA_H', '#define CI_DATA_H', '']
h.append('enum { ' + ', '.join('H_%s' % k.upper() for k, _ in HEADER) + ', H_COUNT };')
h.append('#define BANK_MAX %d' % max(len(b) for b in banks))
h.append('#define SPRITE_COUNT %d' % len(SPR))
h.append('#define BANK_COUNT %d' % len(banks))
h.append('#define STREAM_MAX %d' % max(r[3] for r in stream_recs))
h.append('#define SPRITE_W_MAX %d' % max(r['key'][3] for r in SPR))
h.append('#define LZMA_DICT %d' % DICT)
h.append('enum { SYM_NONE, SYM_BITMAP, SYM_CLIP, SYM_SHAPE, SYM_CONT };')
h.append('enum { SF_STATIC = 1 };')
h.append('enum { K_ABSENT = 1, K_MAT = 2, K_ALPHA = 4, K_REG = 8, K_CLIP = 16, K_NAME = 32, K_HIDDEN = 64, K_KIND = 128 };')
h.append('enum { CK_SYM, CK_SHAPE, CK_TEXT };')
h.append('enum { MODE_INDEPENDENT, MODE_SINGLE, MODE_SYNCHED };')
h.append('enum { ACT_STOP = 1, ACT_DISPATCH_PARENT, ACT_DISPATCH, ACT_PLAY };')
h.append('enum { T_INT = 1, T_FLOAT, T_BOOL, T_STR, T_VEC, T_SYM };')
h.append('enum { E_VAR, E_STR, E_NUM, E_TRUE, E_FALSE, E_EQ, E_NE, E_LT, E_GT, E_LE, E_GE, E_AND, E_OR, E_NOT, E_ADD, E_END };')
h.append('enum { ' + ', '.join('LIB_%s' % n.upper() for n, _ in LIBS) + ', LIB_COUNT };')
h.append('enum { ' + ', '.join('SHEET_%s' % cname(f.split('-')[0]).upper() for f in SHEETS) + ' };')
h.append('')
h.append('/* components and their fields */')
h.append('enum { ' + ', '.join('C_%s' % cname(k) for k in COMPS) + ', C_COUNT };')
h.append('enum { ' + ', '.join('F_%s' % cname(k) for k in FIELDS) + ', F_COUNT };')
h.append('')
h.append('/* storage variables */')
h.append('enum { ' + ', '.join('V_%s' % cname(k) for k in VARS) + ', V_COUNT };')
h.append('')
h.append('/* symbols: S_<library>_<name> */')
seen_names = set()
for i, (lib, k) in enumerate(SYMS):
    nm = 'S_%s_%s' % (lib, cname(k))
    assert nm not in seen_names, nm
    seen_names.add(nm)
    h.append('#define %s %d' % (nm, i))
h.append('#define SYM_COUNT %d' % len(SYMS))
h.append('')
h.append('#endif')
open(os.path.join(ROOT, 'src/data.h'), 'w').write('\n'.join(h) + '\n')

# component / field / var names for the C side (debug and lookups by name)
names = ['/* Generated by tools/pack.py. Do not edit. */', '#include "data.h"',
         'const char *const comp_names[C_COUNT] = {' + ', '.join('"%s"' % k for k in COMPS) + '};',
         'const char *const var_names[V_COUNT] = {' + ', '.join('"%s"' % k for k in VARS) + '};']
open(os.path.join(ROOT, 'src/names.c'), 'w').write('\n'.join(names) + '\n')

# ------------------------------------------------------------------ report
tot_banks = sum(b[1] for b in bank_offs)
tot_streams = sum(r[2] for r in stream_recs)
print('symbols %d, sprites %d (%d banks %d KB, %d streams %d KB), mats %d, strings %d, exprs %d, dialog nodes %d, vars %d' % (
    len(SYMS), len(SPR), len(banks), tot_banks // 1024, len(streams), tot_streams // 1024, len(MATS), len(strings), len(EXPRS), len(dlg_index), len(VARS)))
print({k: v // 1024 for k, v in STATS.items()})
if SLOT_CONFLICTS:
    print('clips whose frames disagree on stacking: %d pairs' % len(SLOT_CONFLICTS))
print('data.bin: %d KB (strings %d KB)' % (len(final) // 1024, len(str_data) // 1024))

if os.environ.get('PACK_STATS'):
    clip_lib = Counter(); seen = set()
    off_to_len = {off: len(data) for data, off in clip_dedupe.items()}
    for (t, v), (lib, k) in zip(sym_table, SYMS):
        if t == SYM_CLIP and v not in seen:
            seen.add(v)
            clip_lib[lib] += off_to_len[v]
    print('clips by lib (KB):', {k: v // 1024 for k, v in clip_lib.most_common()})
    bank_lib = Counter()
    for b_, raw in enumerate(banks):
        m = bank_members[b_]
        tot = sum(SPR[sp]['key'][3] * SPR[sp]['key'][4] for _, sp in m) or 1
        for _, sp in m:
            share = SPR[sp]['key'][3] * SPR[sp]['key'][4] / tot * bank_offs[b_][1]
            bank_lib[','.join(sorted(SPR[sp]['libs'])) or '-'] += share
    print('banks by lib (KB):', {k: int(v) // 1024 for k, v in bank_lib.most_common()})
    unused = [sp for sp in range(len(SPR)) if sp not in seen_spr]
    print('sprites never reached from a clip:', len(unused))

if os.environ.get('PACK_REPORT'):
    by = Counter()
    for sp in streams:
        libs = ','.join(sorted(SPR[sp]['libs']))
        by[libs] += sum(st[1] for st in stream_off[sp])
    print('streams by lib (KB):', {k: v // 1024 for k, v in by.most_common()})
    by = Counter()
    for sp, rec in enumerate(spr_rec):
        if rec:
            by[','.join(sorted(SPR[sp]['libs']))] += 1
    bb = Counter()
    for b, raw in enumerate(banks):
        pass
    print('strings: total bytes', len(str_data))

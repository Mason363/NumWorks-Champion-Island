#!/usr/bin/env python3
"""Checks of Champion Island on a computer (make test): the host build, with
AddressSanitizer and UBSan, in scenes that once went wrong.

    python3 tests/check.py build/play
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

PLAY = sys.argv[1] if len(sys.argv) > 1 else "build/play"
failures = []


def write_save(path, values):
    """A save as store.c writes it: "CI1", then key, type and value (bool, number or text)."""
    out = bytearray(b"CI1")
    for k, v in values.items():
        out += bytes([len(k)]) + k.encode()
        if isinstance(v, bool):
            out += bytes([2, v])
        elif isinstance(v, (int, float)):
            out += bytes([3]) + struct.pack("<f", v)
        else:
            out += bytes([4, len(v)]) + v.encode()
    open(path, "wb").write(out)


def run(scene, frames, keys="", shots=(), env=None, saves=None, save=None):
    """Plays a scene (None: where the game opens) from a fresh save, or from the
    one kept in `saves`, or from these saved values (`save`); returns (stdout,
    stderr, frames saved)."""
    d = tempfile.mkdtemp(prefix="ci_check_")
    os.makedirs(d + "/saves")
    if save:
        write_save(d + "/saves/champion.sav", save)
    e = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
             CI_LEAK="1", CI_CONSIST="1")
    e.update(env or {})
    args = [PLAY] + (["--scene", scene] if scene else []) + ["--frames", str(frames), "--keys", keys, "--out", d,
                                                             "--saves", saves or d + "/saves"]
    if shots:
        args += ["--shots", ",".join(str(s) for s in shots)]
    p = subprocess.run(args, capture_output=True, text=True, env=e, timeout=900)
    imgs = {s: np.asarray(Image.open(f"{d}/shot_{s}.ppm").convert("RGB")) for s in shots if os.path.exists(f"{d}/shot_{s}.ppm")}
    shutil.rmtree(d, ignore_errors=True)
    if p.returncode or "runtime error" in p.stderr or "AddressSanitizer" in p.stderr:
        failures.append(f"{scene}: crashed or a sanitizer error\n{p.stderr[-2000:]}")
    for full in ("NODE POOL FULL", "DRAW LIST FULL"):   # nodes not made, pictures not drawn
        if full in p.stdout:
            failures.append(f"{scene}: " + next(line for line in p.stdout.splitlines() if full in line))
    # every node is under a root: the HUD, the dialogue box, the menus or the scene's (game.c's CI_LEAK) ...
    for line in p.stderr.splitlines():
        if " not under a root" in line and not line.endswith(" 0 not under a root"):
            failures.append(f"{scene}: nodes leaked, {line.strip()}")
            break
    # ... and listed by its parent (play.c's CI_CONSIST)
    if "is not listed by its parent" in p.stderr:
        failures.append(f"{scene}: " + next(l for l in p.stderr.splitlines() if "is not listed" in l))
    return p.stdout, p.stderr, imgs


def check(name, ok, why=""):
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        failures.append(f"{name}: {why}")


def items(stderr):
    """The last frame's draw list (CI_ITEMS): (kind, box) per item."""
    frames, cur = [], None
    for line in stderr.splitlines():
        f = line.split()
        if len(f) < 17 or f[0] != "item":
            continue
        if f[1] == "0":
            cur = []
            frames.append(cur)
        cur.append((int(f[3]), [int(v) for v in f[9:13]], int(f[14])))
    return frames[-1] if frames else []


def random_keys(scene, seed, frames):
    rng = random.Random(seed)
    out, t = [], 0
    while t < frames:
        k = rng.choices(["left", "right", "up", "down", "ok", "back", "pause"], weights=[5, 5, 5, 5, 4, 1, 0.3])[0]
        d = rng.randint(1, 40)
        out.append(f"{t}-{t + d}:{k}")
        t += d + rng.randint(0, 10)
    return ",".join(out)


# Lucky is drawn while he walks inside a house (his walk is shown from the
# second frame of his clip: it used to be missing, only the shadow was left)
_, err, _ = run("interior:teahouse", 50, "30-70:up", env={"CI_ITEMS": "1"})
sprites = [b for k, b, ref in items(err) if k == 0]
shadows = [s for s in sprites if s[3] - s[1] <= 4]


def stands_on(s):   # someone's size, across the shadow, their feet on it
    return any(b[2] - b[0] <= 48 and b[1] < s[1] and s[1] - 2 <= b[3] <= s[3] + 2 and b[0] <= s[2] and b[2] >= s[0]
               for b in sprites if b is not s)


check("Lucky walks inside a house", bool(shadows) and all(stands_on(s) for s in shadows), "a shadow with nobody on it")

# Exit in a sport's pause menu goes back to the island and stays there (the
# OK that chose it, let go on the island, used to open the door again)
_, err, _ = run("pingpong", 1100, "600:ok,900:back,910:down,920:down,930:down,940-943:ok", env={"CI_LOG": "1"})
scenes = [line.split(" scene ")[1] for line in err.splitlines() if " scene " in line]
check("Exit stays on the island", scenes[-1:] == ["overworld"] and "menu pause quit" in err, f"scenes {scenes}")

# the island fills the screen, the sports keep the doodle's frame
_, _, img = run("overworld@hub", 40, shots=[39])
check("the island fills the screen", img[39][:30].any() and img[39][210:].any(), "the bands above and below are black")
_, _, img = run("pingpong", 40, shots=[39])
check("a sport keeps its frame", not img[39][:30].any() and not img[39][210:].any(), "something is drawn above or below")

# while the camera scrolls, what stands on the island moves with the ground
# (the statues and lanterns of the hub used to shake a pixel against it: they
# were placed with the camera's fraction, the layer at its whole pixel)
def shaken(a, b):
    """16 px blocks of frame b a pixel off where the ground's move from frame a puts them."""
    a, b = a.astype(int), b.astype(int)
    _, dx, dy = max(((b[40:200, 20:300] == a[40 + y:200 + y, 20 + x:300 + x]).all(-1).mean(), x, y)
                    for y in range(-6, 7) for x in range(-6, 7))
    n = 0
    for y in range(16, 208, 16):
        for x in range(16, 288, 16):
            if 112 <= x < 208 and 72 <= y < 168:
                continue   # Lucky
            B = b[y:y + 16, x:x + 16]
            if (B == a[y + dy:y + dy + 16, x + dx:x + dx + 16]).all():
                continue
            n += any((B == a[y + dy + j:y + dy + j + 16, x + dx + i:x + dx + i + 16]).all()
                     for j in (-1, 0, 1) for i in (-1, 0, 1) if i or j)
    return n


_, _, img = run("overworld@hub", 60, "20-58:right,20-58:down", shots=range(20, 59))
off = [f for f in range(21, 59) if f in img and f - 1 in img and shaken(img[f - 1], img[f])]
check("the island scrolls in one piece", len(img) == 39 and not off, f"things a pixel off the ground in frames {off}")

# once the climbing scroll is won, Lucky's statue stands on the pedestal at the
# top of the plaza (the champion's statue under him is masked down to its
# pedestal: drawn whole, it hid him)
def lucky_on_pedestal(store):
    _, _, img = run("overworld@hub", 40, shots=[39], env={"CI_POS": "-11,-60", "CI_STORE": store})
    r = img[39][40:100, 136:184].astype(int)
    return int(((r[..., 0] > 200) & (r[..., 1] > 90) & (r[..., 1] < 200) & (r[..., 2] < 90)).sum())


won, not_won = lucky_on_pedestal("climbing_rating=3"), lucky_on_pedestal("climbing_rating=2")
check("Lucky's statue once the climbing scroll is won", won > 40 and not_won == 0, f"orange pixels: {won} won, {not_won} not won")
# after the ending, the glow (nAa, the doodle's whole screen) covers the whole
# island view like the doodle's screen: its top is 168/255 pink and its
# faintest band (21/255) reaches the middle and, down the right edge, the
# bottom (it used to stop 60 rows short, the top 180 rows only). Pixels are
# gfx.c's blend of the glow's colour over the same frame drawn without it.
_, _, glow = run("overworld@start", 40, shots=[39], env={"CI_ENDING": "1"})
_, _, bare = run("overworld@start", 40, shots=[39], env={"CI_ENDING": "1", "CI_NOGLOW": "1"})


def glowed(y0, y1, x0, x1, rgb, a):
    """Share of the pixels the glow changes that are rgb blended at a/32 (petals aside), and their count."""
    to565 = np.array([31, 63, 31]) / 255
    g = np.rint(glow[39][y0:y1, x0:x1] * to565).astype(int)
    b = np.rint(bare[39][y0:y1, x0:x1] * to565).astype(int)
    f = np.array([rgb[0] >> 3, rgb[1] >> 2, rgb[2] >> 3])
    want = b + (f - b) * a // 32
    changed = (want != b).any(2)
    return ((want == g).all(2) & changed).sum() / max(changed.sum(), 1), changed.sum()


for name, box, rgb, a in (("over the top", (0, 15, 40, 320), (244, 213, 251), 21),
                          ("to the middle", (134, 145, 20, 280), (203, 162, 144), 3),
                          ("down to the bottom right", (220, 240, 318, 320), (203, 162, 144), 3)):
    share, n = glowed(*box, rgb, a) if 39 in glow and 39 in bare else (0, 0)
    check(f"the ending's glow {name}", n >= 10 and share > .9, f"{share:.0%} of {n} pixels")

# nodes are all freed when a scene ends (node_free used to leave some 50 of
# the climbing wall's 130 children behind, every time)
before = len(failures)
run("climbing", 1200, "60:ok,300:back,310:down,320:down,330:ok,400:ok,700:back,710:down,720:down,730:down,740:ok,"
    "900-960:down,1000:back,1050:back")
print(("ok   " if len(failures) == before else "FAIL ") + "no nodes left behind by a scene")

# the climbing wall is made whole (its nodes used to run out: 40 to 62 missing)
for v in ("climbing", "climbing:hard"):
    before = len(failures)
    run(v, 60)
    print(("ok   " if len(failures) == before else "FAIL ") + f"{v} has all its nodes")

# the dialogue box breaks its text as the doodle does (at the text's lineWidth
# in its own 10 px) and grows to it: the Trophy Master's five lines used to be
# seven, spilling out of the box
_, err, _ = run("interior:trophyRoom", 580, "20-80:up,90:ok,190:ok,290:ok,390:ok,490:ok", env={"CI_ITEMS": "1"})
last = items(err)
texts = [b for k, b, ref in last if k == 4 and b[1] < 40]
bases = [b for k, b, ref in last if k == 1 and b[0] < 10 and b[2] > 300]
check("a long dialogue fits its box", texts and bases and texts[0][3] < bases[0][3] and texts[0][3] - texts[0][1] < 90,
      f"text {texts} box {bases}")

# the map shows the player's team and the scrolls won (their storageSprites:
# it used to show the blue team and no scroll, whatever the save)
won = {"TUTORIAL_DONE": True, "PLAYER_TEAM": "red", "pingpong_score": 5000, "pingpong_rating": 3}
_, _, img = run("overworld@hub", 60, "30:back", shots=[59], save=won)
_, _, img0 = run("overworld@hub", 60, "30:back", shots=[59], save={"TUTORIAL_DONE": True, "PLAYER_TEAM": "red"})
icon = img[59][155:180, 250:272].reshape(-1, 3).mean(0) if 59 in img else np.zeros(3)
scroll = np.abs(img[59][135:160, 60:85].astype(int) - img0[59][135:160, 60:85].astype(int)).mean() if 59 in img and 59 in img0 else 0
check("the map shows the team and the scrolls", icon[2] < icon[0] / 2 and scroll > 2, f"team icon {icon}, scroll {scroll}")

# a menu's shade darkens the whole screen on the island (the skip menu's is a
# quarter pixel short of the stage: the bands above and below stayed bright)
_, _, img = run("overworld@hub", 40, "20:back", shots=[39])
band = np.concatenate([img[39][:30], img[39][210:]]).mean() if 39 in img else 255
check("the skip menu shades the whole island", band < 60, f"bands at {band:.0f}")

# The climbing backdrop (the vista behind the cliff) is the picture itself at
# every height: the sea at the top of the holes halfway up is its deep blue
# (it used to be a flat teal below the picture's row 400), and in the hard
# variant the forest's temples are where the picture has them (it used to be
# a strip of the forest repeated). CI_CAM puts the camera (the map) there.
def cam(x, y, f0=130):
    return {"CI_CAM": f"{f0}:1000:{x}:{x}:0:{y}:{y}:0"}


def view_of(img, shot):
    return img[shot][30:210].astype(int) if shot in img else np.zeros((180, 320, 3), int)


sea = view_of(run("climbing", 136, "60:ok", shots=[135], env=cam(120, 2493))[2], 135)[0:20, 40:80].reshape(-1, 3)
colours, counts = np.unique(sea, axis=0, return_counts=True)
top = colours[counts.argmax()]
check("the climbing vista's sea halfway up", top[2] > top[1], f"its main colour is {top.tolist()}")


def roofs(v):   # the temples' grey-blue roofs
    return int(((abs(v[..., 0] - v[..., 1]) < 25) & (v[..., 2] > v[..., 1] + 8) & (v[..., 0] > 70) & (v[..., 0] < 190)).sum())


view = view_of(run("climbing:hard", 136, "60:ok", shots=[135], env=cam(120, 2403))[2], 135)
here, there = roofs(view[100:160, 280:320]), roofs(view[20:80, 280:320])
check("the climbing vista's forest in the hard variant", here > 400 and there < 100,
      f"roofs: {here} px where the picture has them, {there} where it has none")

# the rugby end zone's grass glows (its clip plays alpha 0 to 1 to 0): it used
# to stay at its first frame, unseen
shots = [230, 242, 254, 266, 278]
_, _, img = run("rugby", 280, "60:ok", shots=shots, env=cam(-3643, 266, 200))
green = 0.0
for s in shots:
    v = view_of(img, s)[20:160, 290:320]
    green = max(green, ((v[..., 1] > v[..., 0] + 30) & (v[..., 1] > v[..., 2] + 30)).mean())
check("the rugby end zone shows", green > .5, f"at most {green:.0%} of it green")

# no seam between skate park 2's ground tiles a quarter pixel apart (CreateJS
# snaps each one's offset to a whole pixel: they meet)
col = view_of(run("skate:park2", 226, "60:ok", shots=[225], env=cam(3068, -1451, 220))[2], 225)[36:180, 91].sum(1)
check("no seam in skate park 2's ground", (col < 60).sum() < 20, f"{(col < 60).sum()} dark pixels in column 91")

# the skate tutorial's bottom right houses show with the camera at the bottom
# (its index of children is cut short: what lies past its last row is kept
# there, and a query past that row used to skip it)
shop = view_of(run("skate:tutorial", 226, "60:ok", shots=[225], env=cam(-2342, -1325, 220))[2], 225)[40:130, 0:90]
kinds = len(np.unique(shop.reshape(-1, 3), axis=0))
check("the skate tutorial's corner houses", kinds > 25, f"{kinds} colours where the houses are (the ground has 8)")

# the island kept up as the camera moves is the island drawn from scratch
# (play.c's CI_DIFF; CI_PATH flies the player through the streets by the
# skate park and past the tutorial's signs): the map's live children were
# made from their origin alone (the clips' nominal bounds are not in the
# data), so houses, signs and lanterns showed up late at the edges
out, err, _ = run("overworld@hub", 720, env={"CI_PATH": "4;-450,860;-1300,860;-1300,650;-700,650;-700,780;150,780;150,600", "CI_DIFF": "3"})
bad = [] if "scene overworld:" in out else ["not on the island"]
for line in err.splitlines():
    m = re.match(r"diff f\d+: (\d+) px .*, (\d+) children missing, (\d+) items left for later, layer (\d+) px", line)
    if m and (int(m[2]) or int(m[4]) or (int(m[1]) and not int(m[3]))):   # (what the cache leaves for the next frame may differ)
        bad.append(line)
check("the island drawn as the camera moves is the island drawn from scratch", not bad, "\n".join(bad[:5]))

# once the view is still in the busiest streets, the scenery drawn again in
# front of someone (a house before a sign) is all there: its masks were not
# counted in what a frame needs from the cache, and one left out was not
# tried again
_, err, _ = run("overworld@hub", 420, env={"CI_PATH": "4;-1240,660;-1380,660;-1170,590;-750,450;-680,450", "CI_HOLD": "30"})
held = [line for line in err.splitlines() if line.startswith("held ")]
check("the busiest streets are whole once the view is still", len(held) == 5 and all(line.endswith(" left 0") for line in held), "\n".join(held))

# standing still in that street (an animated sign and someone walking redraw
# a few bands), the two big buildings and the scenery in front need more than
# the cache: the bigger one is read as the bands go down, the others decoded
# once each (they used to push each other out band after band, 600 KB and
# more decoded in one frame, a frame of 150 ms on the calculator)
_, err, _ = run("overworld@conveniencestore2", 420, "200-215:left", env={"CI_TUTORIAL_DONE": "1", "CI_ZFRAME": "1"})
kb = {int(m[1]): float(m[2]) for m in re.finditer(r"^f(\d+) decoded ([\d.]+) KB", err, re.M)}
still = [kb.get(f, 0) for f in range(240, 415)]
check("standing still in the busiest street decodes little", sum(still) < 6500 and max(still) < 450,
      f"{sum(still):.0f} KB in all, at most {max(still):.0f} KB in a frame")

# random keys where a crash once was (the trophy room's entities outnumbered
# their state records), and in every kind of scene
for scene in ("interior:trophyRoom", "overworld@hub", "overworld@pingponghouse1", "skate:park2", "rugby", "archery"):
    before = len(failures)
    run(scene, 1500, random_keys(scene, 1, 1500))
    print(("ok   " if len(failures) == before else "FAIL ") + f"random keys in {scene}")

# the player can always move, however he goes in and out of doors (play.c's
# CI_WANDER plays at random: arrows held through the change, OK held at a door,
# the map opened as a room fades in...): the island's walls stayed in the
# physics world, filling it after a few doors, and the player got no body, in
# a room or back on the island
for place, seed in (("trophy", 0), ("pingpongdojo", 0), ("locksmith", 0), ("koma1", 1)):
    out, err, _ = run(f"overworld@{place}", 3000, env={"CI_WANDER": str(seed), "CI_STUCK": "1", "CI_LOG": "1"})
    rooms = sum(" scene interior" in line for line in err.splitlines())
    stuck = [line for line in out.splitlines() if line.startswith(("STUCK", "BODY"))]
    check(f"in and out of the doors from {place}", rooms >= 8 and not stuck, f"{rooms} rooms, {stuck[:2]}")


def at(out, frame):
    """Where the player is at a frame (play.c's CI_STUCK=2)."""
    for line in out.splitlines():
        if line.startswith(f"f{frame} "):
            x, y = line.split(" at ")[1].split()[0].split(",")
            return float(x), float(y)
    return None


# a room entered by its far door has the player at that door (a hallway, back
# from its dojo), and the game opens on the island where he went in: a room's
# exit saved a place of the room, and the island then opened on the dock
saves = tempfile.mkdtemp(prefix="ci_check_")
out, err, _ = run("overworld@pingpongdojo", 300, "10-25:up,30:ok,60-140:up,150-175:down,280:home",
                  env={"CI_TUTORIAL_DONE": "1", "CI_STUCK": "2", "CI_LOG": "1"}, saves=saves)
p = at(out, 200)
check("back from the dojo at the hallway's far end", "@pingpongdojohallwayn" in err and p and p[1] < 60, f"at {p}")
out, _, _ = run(None, 20, env={"CI_STUCK": "2"}, saves=saves)
p = at(out, 10)
check("the game opens at the door he went in by", p and abs(p[0] - 1045) < 30 and abs(p[1] + 1080) < 30, f"at {p}")
shutil.rmtree(saves, ignore_errors=True)

if failures:
    print("\n".join(failures))
    sys.exit(1)
print("all checks passed")

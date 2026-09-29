#!/usr/bin/env python3
"""Checks of Champion Island on a computer (make test): the host build, with
AddressSanitizer and UBSan, in scenes that once went wrong.

    python3 tests/check.py build/play
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

PLAY = sys.argv[1] if len(sys.argv) > 1 else "build/play"
failures = []


def run(scene, frames, keys="", shots=(), env=None):
    """Plays a scene from a fresh save; returns (stdout, stderr, frames saved)."""
    d = tempfile.mkdtemp(prefix="ci_check_")
    os.makedirs(d + "/saves")
    e = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
             CI_LEAK="1", CI_CONSIST="1")
    e.update(env or {})
    args = [PLAY, "--scene", scene, "--frames", str(frames), "--keys", keys, "--out", d, "--saves", d + "/saves"]
    if shots:
        args += ["--shots", ",".join(str(s) for s in shots)]
    p = subprocess.run(args, capture_output=True, text=True, env=e, timeout=900)
    imgs = {s: np.asarray(Image.open(f"{d}/shot_{s}.ppm").convert("RGB")) for s in shots if os.path.exists(f"{d}/shot_{s}.ppm")}
    shutil.rmtree(d, ignore_errors=True)
    if p.returncode or "runtime error" in p.stderr or "AddressSanitizer" in p.stderr:
        failures.append(f"{scene}: crashed or a sanitizer error\n{p.stderr[-2000:]}")
    if "NODE POOL FULL" in p.stdout:
        failures.append(f"{scene}: {p.stdout.strip().splitlines()[0]}")
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

# random keys where a crash once was (the trophy room's entities outnumbered
# their state records), and in every kind of scene
for scene in ("interior:trophyRoom", "overworld@hub", "overworld@pingponghouse1", "skate:park2", "rugby", "archery"):
    before = len(failures)
    run(scene, 1500, random_keys(scene, 1, 1500))
    print(("ok   " if len(failures) == before else "FAIL ") + f"random keys in {scene}")

if failures:
    print("\n".join(failures))
    sys.exit(1)
print("all checks passed")

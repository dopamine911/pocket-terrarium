#!/usr/bin/env python3
"""Write the C/Python L1 encoder parity fixture (PLAN.md R4). Stdlib only.

  python3 model/make_l1_parity.py            # -> model/testdata/l1_parity.jsonl
  python3 model/make_l1_parity.py --check    # regenerate in memory; exit 1 if the file differs

One JSON object per line: {"snap": {...}, "line": "<the L1 state line>"}. The
line comes from gen_traces.encode_l1_snap, the encoder the traces are made
with; the C selftest feeds every snap to advisor_core_encode_l1 and must get
the same bytes. snap fields (all present on every line):

  pack       1 = leopard gecko, 250 = the A16 test pack (the gecko, every t_* +6 C)
  zone       1-6, precomputed (the encoder does not compute zones)
  x y        lizard position, px;  heading  radians (any value: the clock wraps it)
  body_c     body temperature C (the encoder maps it through the pack, then drive9)
  hunger energy stress curiosity trust bored   0..10 (drive9)
  bold social                                  0..1  ((int)(v * 9 + 0.5))
  stage shed last phase                        words
  prey other warm shelter   [x, y] or null;  in_shelter  bool (true: "shelter near 12" whatever the point)
  wall       the nearest glass contact point [x, y] or null (shown only when near/mid)

Deterministic: a fixed seed, no clock, no hash order; --check proves it.
Boundary-safe, so float (C) against double (Python) cannot flip a word: no
drive, trait or heat value within MARGIN of a digit boundary, no distance
within DIST_MARGIN px of a bucket edge, no direction within CLOCK_MARGIN_DEG of
a clock half-hour - except body_c exactly at a pack's t_cold / t_lo / t_hi /
t_hot, where either side of the piecewise map gives heat 0 / 5 / 7 / 9. Every
line is also re-encoded from float32-rounded inputs and must not change.
Numbers are plain decimals (never exponent form), so a small C reader will do.
"""
import argparse
import json
import math
import os
import random
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_traces as gt  # noqa: E402

SEED = 20261104
N_RANDOM = 400
MARGIN = 0.02            # drive/heat units (the contract asks for > 1e-3)
TRAIT_MARGIN = 0.02      # in units of v * 9
DIST_MARGIN = 0.5        # px from 70 / 180 / 380
CLOCK_MARGIN_DEG = 1.0   # from a clock half-hour (the contract asks for > 0.5)
KINDS = ["near", "mid", "far", None, "beyond"]   # None = no such thing; beyond = present but past far
DIST_RANGE = {"near": (3.0, 68.0), "mid": (72.0, 178.0), "far": (182.0, 378.0), "beyond": (382.0, 560.0)}
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "testdata", "l1_parity.jsonl")
EXAMPLE = ("zone 4 hunger 7 energy 5 stress 2 curiosity 6 heat 3 bold 4 social 2 stage adult trust 6 bored 1 "
           "shed no prey mid 2 other none warm far 11 shelter near 7 wall clear last explore phase active")


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def near_int(v, lo=1, hi=9):
    """distance from v to the nearest digit boundary lo..hi"""
    return min(abs(v - k) for k in range(lo, hi + 1))


def drive_value(rng, digit):
    if digit == 9:
        return round(rng.uniform(9.05, 10.0), 3)
    return round(rng.uniform(digit + 0.05, digit + 0.95), 3)


def trait_value(rng, digit):
    v = (digit + rng.uniform(-0.4, 0.4)) / 9.0
    return round(min(1.0, max(0.0, v)), 4)


def body_value(rng, pack, digit):
    """a body temperature whose heat lands inside `digit`, away from its edges"""
    p = gt.PACKS[pack]
    r = rng.random()
    if digit == 0 and r < 0.25:
        return round(rng.uniform(p.t_cold - 9.0, p.t_cold - 0.05), 3)        # colder than it can stand
    if digit == 9 and r < 0.25:
        return round(rng.uniform(p.t_hot + 0.05, p.t_hot + 7.0), 3)          # past dangerous
    lo = 0.05 if digit == 0 else digit + 0.05
    hi = 9.999 if digit == 9 else digit + 0.95
    return round(gt.body_c_of(pack, rng.uniform(lo, hi)), 3)


def point(rng, x, y, heading, kind, clock):
    """a point `kind` away from (x, y) at `clock` relative to heading (None for kind None)"""
    if kind is None:
        return None
    d = rng.uniform(*DIST_RANGE[kind])
    a = heading + math.radians(clock * 30.0 + rng.uniform(-13.0, 13.0))
    return [round(x + d * math.cos(a), 2), round(y + d * math.sin(a), 2)]


def safe_point(s, pt):
    """the point's distance and direction are clear of every bucket edge and clock half-hour"""
    if pt is None:
        return True
    d = math.hypot(pt[0] - s["x"], pt[1] - s["y"])
    if min(abs(d - e) for e in (gt.NEAR, gt.MID, gt.FAR)) < DIST_MARGIN:
        return False
    rel = math.degrees((math.atan2(pt[1] - s["y"], pt[0] - s["x"]) - s["heading"]) % (2 * math.pi))
    return abs(rel % 30.0 - 15.0) > CLOCK_MARGIN_DEG


def safe(s, exact_body=False):
    for k in gt.L1_DRIVES:
        if near_int(s[k]) < MARGIN:
            return False
    for k in ("bold", "social"):
        if min(abs(s[k] * 9 - (j + 0.5)) for j in range(9)) < TRAIT_MARGIN:
            return False
    if not exact_body and near_int(gt.heat_of(s["pack"], s["body_c"])) < MARGIN:
        return False
    return all(safe_point(s, s[k]) for k in gt.L1_SIGHTS + ["wall"])


def snap_f32(s):
    """the snap as the C sees it: every float rounded to float32"""
    o = dict(s)
    for k, v in s.items():
        if isinstance(v, float):
            o[k] = f32(v)
        elif isinstance(v, list):
            o[k] = [f32(c) for c in v]
    return o


def base_snap(rng, i, pack):
    """one random snapshot; the sightings and the wall cycle so that every
    (bucket, clock) pair turns up for each of them within 60 lines"""
    zone = i % 6 + 1
    col, row = (zone - 1) % 3, (zone - 1) // 3
    x = round(rng.uniform(col * gt.TANK_W / 3 + 4, (col + 1) * gt.TANK_W / 3 - 4), 2)
    y = round(rng.uniform(row * gt.TANK_H / 2 + 4, (row + 1) * gt.TANK_H / 2 - 4), 2)
    r = rng.random()
    heading = rng.uniform(0, 2 * math.pi) if r < 0.75 else \
        rng.uniform(-2 * math.pi, 0) if r < 0.9 else rng.uniform(2 * math.pi, 4 * math.pi)
    heading = round(heading, 5)
    s = {"pack": pack, "zone": zone, "x": x, "y": y, "heading": heading,
         "body_c": body_value(rng, pack, rng.randrange(10))}
    for k in gt.L1_DRIVES:
        s[k] = drive_value(rng, rng.randrange(10))
    s["bold"] = trait_value(rng, rng.randrange(10))
    s["social"] = trait_value(rng, rng.randrange(10))
    s["stage"] = rng.choice(gt.STAGES_L1)
    s["shed"] = rng.choice(gt.SHED_L1)
    s["last"] = rng.choice(gt.GOALS_L1)
    s["phase"] = rng.choice(gt.PHASES_L1)
    for j, k in enumerate(gt.L1_SIGHTS + ["wall"]):
        c = (i + 17 * j) % 60
        s[k] = point(rng, x, y, heading, KINDS[c // 12], c % 12 + 1)
        if k == "shelter":
            s["in_shelter"] = rng.random() < 0.2
    return s


def build():
    rng = random.Random(SEED)
    rows = []

    def add(s, exact_body=False):
        assert safe(s, exact_body), s
        line = gt.encode_l1_snap(s)
        assert not gt.check_l1_line(line), (line, gt.check_l1_line(line))
        assert gt.encode_l1_snap(snap_f32(s)) == line, ("float32 inputs change the line", s)
        rows.append({"snap": s, "line": line})

    def fresh(i, pack):
        while True:
            s = base_snap(rng, i, pack)
            if safe(s):
                return s

    # 1. the schema's own example line (model/schema-lizard.md "State line")
    s = fresh(3, 1)
    x, y = 100.0, 250.0

    def at(d, deg):
        return [round(x + d * math.cos(math.radians(deg)), 2), round(y + d * math.sin(math.radians(deg)), 2)]
    s.update(zone=4, x=x, y=y, heading=0.0, body_c=25.6, hunger=7.4, energy=5.5, stress=2.3, curiosity=6.6,
             trust=6.2, bored=1.7, bold=0.45, social=0.22, stage="adult", shed="no", last="explore",
             phase="active", prey=at(120, 60), other=None, warm=at(250, 330), shelter=at(40, 210),
             in_shelter=False, wall=None)
    add(s)
    assert rows[0]["line"] == EXAMPLE, rows[0]["line"]

    # 2. body_c exactly on each pack's band edges (heat 0 / 5 / 7 / 9 from either side), and far outside it
    for pack in sorted(gt.PACKS):
        p = gt.PACKS[pack]
        for t in (p.t_cold, p.t_lo, p.t_hi, p.t_hot):
            s = fresh(len(rows), pack)
            s["body_c"] = float(t)
            add(s, exact_body=True)
        for t in (p.t_cold - 10.0, p.t_hot + 10.0):
            s = fresh(len(rows), pack)
            s["body_c"] = t
            add(s)

    # 3. the extremes of every drive and trait
    for v, tv in ((0.0, 0.0), (10.0, 1.0)):
        s = fresh(len(rows), 1)
        for k in gt.L1_DRIVES:
            s[k] = v
        s["bold"] = s["social"] = tv
        add(s)

    # 4. shelter: inside with no point, inside with a point past far or at mid, outside with no
    #    hide at all; and a lizard that senses nothing (every sighting null, no wall)
    for in_sh, kind in ((True, None), (True, "beyond"), (True, "mid"), (False, None)):
        s = fresh(len(rows), 250)
        s["in_shelter"] = in_sh
        s["shelter"] = point(rng, s["x"], s["y"], s["heading"], kind, rng.randint(1, 12))
        while not safe(s):
            s["shelter"] = point(rng, s["x"], s["y"], s["heading"], kind, rng.randint(1, 12))
        add(s)
    s = fresh(len(rows), 1)
    for k in gt.L1_SIGHTS + ["wall"]:
        s[k] = None
    s["in_shelter"] = False
    add(s)

    # 5. the bulk: random states, the two packs alternating
    for i in range(N_RANDOM):
        add(fresh(i, 1 if i % 2 == 0 else 250))
    return rows


def coverage(rows):
    """every value of every field turns up (asserted, so a seed change cannot quietly lose one)"""
    seen = {}
    for r in rows:
        w = r["line"].split()
        i = 0
        while i < len(w):
            key, v = w[i], w[i + 1]
            if v in ("near", "mid", "far"):
                v = f"{v} {w[i + 2]}"
                i += 1
            seen.setdefault(key, set()).add(v)
            i += 2
        seen.setdefault(f"heat@{r['snap']['pack']}", set()).add(w[w.index("heat") + 1])
        seen.setdefault("in_shelter", set()).add(r["snap"]["in_shelter"])
    digits = {str(d) for d in range(10)}
    want = {k: digits for k in gt.L1_DRIVES + ["heat", "bold", "social"] + [f"heat@{p}" for p in gt.PACKS]}
    want.update(zone={str(z) for z in range(1, 7)}, stage=set(gt.STAGES_L1), shed=set(gt.SHED_L1),
                last=set(gt.GOALS_L1), phase=set(gt.PHASES_L1), in_shelter={True, False})
    sights = {"none"} | {f"{b} {c}" for b in ("near", "mid", "far") for c in range(1, 13)}
    want.update({k: sights for k in gt.L1_SIGHTS})
    want["wall"] = {"clear"} | {f"{b} {c}" for b in ("near", "mid") for c in range(1, 13)}
    missing = {k: sorted(map(str, v - seen.get(k, set()))) for k, v in want.items() if v - seen.get(k, set())}
    assert not missing, f"fixture misses {missing}"


def render(rows):
    text = "".join(json.dumps(r) + "\n" for r in rows)
    assert not re.search(r"\d[eE][-+]?\d", text), "a number in exponent form (keep the C reader simple)"
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true", help="regenerate in memory and compare with --out")
    args = ap.parse_args()
    rows = build()
    coverage(rows)
    text = render(rows)
    if args.check:
        try:
            old = open(args.out).read()
        except OSError:
            sys.exit(f"{args.out}: missing")
        if old != text:
            sys.exit(f"{args.out}: differs from a fresh build (run without --check to rewrite it)")
        print(f"{args.out}: up to date ({len(rows)} lines)")
        return
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(text)
    packs = {p: sum(1 for r in rows if r["snap"]["pack"] == p) for p in sorted(gt.PACKS)}
    print(f"wrote {args.out}: {len(rows)} lines (packs {packs}), every field value covered")


if __name__ == "__main__":
    main()

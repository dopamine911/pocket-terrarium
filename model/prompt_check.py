#!/usr/bin/env python3
"""Check a teacher prompt BEFORE the overnight run. Stdlib only.

The v3 overnight taught us the expensive way that one sentence in the teacher
prompt reshapes every label (P(follow | social>=8) fell 0.50 -> 0.18). This
asks the teacher ~300 questions on a fixed diagnostic panel and prints the
conditional rates the student will inherit - ~7 minutes with 3 workers, not
8 hours. Use it to compare prompts, or teachers (--model qwen3.5:27b ...).

  python3 prompt_check.py --schema 3                       # current v3 prompt
  python3 prompt_check.py --schema 3 --prompt-file cand.txt # a candidate prompt
  python3 prompt_check.py --schema 3 --model qwen3.5:27b   # another teacher
  python3 prompt_check.py --schema L1 --api openai --workers 4   # the lizard line on the lab's chat-teacher

Panel (per schema line, identity randomized unless the case pins it):
  social9   content fish, social 9 (expect follow_friend high)     x40
  social1   content fish, social 1 (expect follow_friend ~0)        x40
  bold9     content + energetic, bold 9 (expect dart_play present)  x40
  bold1     same, bold 1 (expect dart_play ~0)                      x40
  content   content, mid identity (watch for a single attractor)    x40
  shadowcalm shadow near, stress 0-3 (expect flee high)             x40
  lonely    friend none, content (must not flee / seek food)        x30
  starving  hunger 9, food near (expect seek_food ~1.0)             x30
  trust     content, trust 9 vs trust 0 (v3 only; expect a shade, not a flip) x40
  v4 (--schema 4; the shadow cases are skipped, bored cases added):
  bored9bub  content, bored 9, last visit_bubbles (expect a change, explore up) x40
  bored9fol  social 8, friend near, bored 9, last follow_friend (expect a change) x40
  bored0bub  content, bored 0, last visit_bubbles (expect last mostly KEPT)     x40
  nightbored night, bored 9, last rest (expect rest - boredom never wakes the tank) x30
  L1 (--schema L1): the teacher-side rows of model/schema-lizard.md's acceptance,
  A2-A12, on a content lizard (fed, comfortable, calm, active) with one thing
  pinned per case, x24 each at the default --n 40 (432 calls, ~4 min on
  chat-teacher with 4 workers). The tendencies are PAIRS - the same backgrounds
  twice, one field changed: bold 9 vs 0 (A11), hatchling vs adult at stress 4-6
  (rule 8), shed soon vs no (rule 3). Plus social9near (rule 7), content (no
  attractor), satedprey (a fed lizard's snap) and bored0any (A10 with any last).
  --cases bold9,social9near runs a subset on the very same states.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_traces as gt  # noqa: E402

L1_PASTIMES = ["explore", "investigate", "dig", "rest"]


def state(rng, schema, **pin):
    f = dict(zone=rng.randint(1, 6), hunger=rng.randint(0, 3), energy=rng.randint(5, 9), stress=rng.randint(0, 2),
             curiosity=rng.randint(3, 8), bold=rng.randint(2, 7), social=rng.randint(2, 6),
             stage=rng.choice(["juv", "adult", "adult", "elder"]), trust=rng.randint(2, 7),
             bored=rng.randint(0, 2), food="none", shadow="none", friend=f"{rng.choice(['near','mid'])} {rng.randint(1,12)}",
             bubble=f"{rng.choice(['near','mid','far'])} {rng.randint(1,12)}",
             reef=f"{rng.choice(['mid','far'])} {rng.randint(1,12)}", wall="clear",
             last=rng.choice(["explore", "inspect_reef", "visit_bubbles", "rest"]), time="day")
    f.update(pin)
    ident = f"hunger {f['hunger']} energy {f['energy']} stress {f['stress']} curiosity {f['curiosity']} bold {f['bold']} social {f['social']} stage {f['stage']}"
    tail = (f"food {f['food']} shadow {f['shadow']} friend {f['friend']} bubble {f['bubble']} reef {f['reef']} "
            f"wall {f['wall']} last {f['last']} time {f['time']}")
    if schema >= 4:
        return (f"zone {f['zone']} {ident} trust {f['trust']} bored {f['bored']} food {f['food']} friend {f['friend']} "
                f"bubble {f['bubble']} reef {f['reef']} wall {f['wall']} last {f['last']} time {f['time']}")
    if schema >= 3:
        return f"zone {f['zone']} {ident} trust {f['trust']} {tail}"
    name = rng.choice(["mira", "bolt", "kelp", "nori"])
    fr = f['friend'] if f['friend'] == "none" else f"{rng.choice(['mira','bolt','kelp','nori'])} {f['friend']}"
    return f"fish {name} zone {f['zone']} {ident} {tail.replace('friend ' + f['friend'], 'friend ' + fr, 1)}"


def panel(rng, schema, n):
    P = []
    def add(case, k, **pin):
        for _ in range(int(n * k / 40)):
            P.append((case, state(rng, schema, **pin)))
    add("social9", 40, social=9)
    add("social1", 40, social=1)
    add("bold9", 40, bold=9, energy=9, hunger=rng.randint(0, 2))
    add("bold1", 40, bold=1, energy=9, hunger=rng.randint(0, 2))
    add("content", 40)
    if schema < 4:
        add("shadowcalm", 40, shadow=f"near {rng.randint(1,12)}", stress=rng.randint(0, 3))
    add("lonely", 30, friend="none")
    add("starving", 30, hunger=9, food=f"near {rng.randint(1,12)}")
    if schema >= 3:
        add("trust9", 20, trust=9)
        add("trust0", 20, trust=0)
    if schema >= 4:
        add("bored9bub", 40, bored=9, last="visit_bubbles", bubble=f"near {rng.randint(1,12)}")
        add("bored9fol", 40, bored=9, last="follow_friend", social=8, friend=f"near {rng.randint(1,12)}")
        add("bored0bub", 40, bored=0, last="visit_bubbles", bubble=f"near {rng.randint(1,12)}")
        add("nightbored", 30, bored=9, last="rest", time="night", energy=rng.randint(3, 6), reef=f"near {rng.randint(1,12)}")
    return P


def _sight(rng, kinds):
    k = rng.choice(kinds)
    return "none" if k == "none" else f"{k} {rng.randint(1, 12)}"


def fields_l1(rng, **pin):
    """a content lizard - fed, comfortable, calm, in its active phase - with the
    pinned fields (a value, or a function of rng drawn per state)"""
    f = dict(zone=rng.randint(1, 6), hunger=rng.randint(0, 3), energy=rng.randint(5, 9), stress=rng.randint(0, 2),
             curiosity=rng.randint(3, 8), heat=rng.randint(5, 6), bold=rng.randint(2, 7), social=rng.randint(3, 6),
             stage=rng.choice(["juv", "adult", "adult", "elder"]), trust=rng.randint(2, 7), bored=rng.randint(0, 2),
             shed="no", prey="none", other=_sight(rng, ["near", "mid", "far", "none"]),
             warm=_sight(rng, ["mid", "far"]), shelter=_sight(rng, ["near", "mid", "far"]),
             wall=rng.choice(["clear", f"near {rng.randint(1, 12)}", f"mid {rng.randint(1, 12)}"]),
             last=rng.choice(L1_PASTIMES), phase="active")
    for k, v in pin.items():
        f[k] = v(rng) if callable(v) else v
    return f


def line_l1(f):
    line = " ".join(f"{k} {f[k]}" for k in gt.L1_FIELDS)
    assert not gt.check_l1_line(line), (line, gt.check_l1_line(line))
    return line


def state_l1(rng, **pin):
    return line_l1(fields_l1(rng, **pin))


# the L1 panel: (case, weight per 40 of --n); a pair is two cases on the SAME
# backgrounds that differ in one field, so their difference is the field's effect
L1_PAIRS = {"bold9": "bold0", "hatch5": "adult5", "shedsoon": "shedno"}


def panel_l1(rng, n):
    P = []

    def count(k):
        return max(1, int(n * k / 40))

    def add(case, k, **pin):
        for _ in range(count(k)):
            P.append((case, state_l1(rng, **pin)))

    def add_pair(case_a, case_b, k, a, b, **pin):
        for _ in range(count(k)):
            f = fields_l1(rng, **pin)
            P.append((case_a, line_l1(dict(f, **a))))
            P.append((case_b, line_l1(dict(f, **b))))

    def span(lo, hi):
        return lambda r: r.randint(lo, hi)

    def pick(*words):
        return lambda r: r.choice(words)

    def near_mid(r):
        return f"{r.choice(['near', 'mid'])} {r.randint(1, 12)}"

    def near(r):
        return f"near {r.randint(1, 12)}"
    add("starving", 24, hunger=9, prey=near_mid, heat=span(3, 7))                   # A2
    add("starvecold", 24, hunger=9, prey="none", heat=span(0, 2))                   # A3
    add("cold", 24, heat=span(0, 2), hunger=span(0, 5), stress=span(0, 4))         # A4
    add("hot", 24, heat=9, hunger=span(0, 9), stress=span(0, 6))                    # A5
    add("shedyes", 24, shed="yes", hunger=span(0, 6))                               # A6
    add("sleep", 24, phase="sleep", hunger=span(0, 7), heat=span(1, 7),
        last=pick("rest", "hide", "explore"))                                      # A7
    add("stress", 24, stress=span(8, 9))                                            # A8
    add("bored9", 24, bored=span(8, 9))                                             # A9
    add("bored0any", 24, bored=span(0, 1))                  # report: any last, even one that no longer fits
    # A11: the same content lizards at bold 9 and bold 0 (bored 2-5: neither a rut
    # nor a fresh start, so `last` does not decide it)
    add_pair("bold9", "bold0", 24, {"bold": 9}, {"bold": 0},
             bored=span(2, 5), last=pick(*L1_PASTIMES, "hide"))
    add("social0", 24, social=0, other=near)                                        # A12
    add("social9near", 20, social=span(7, 9), other=near, bored=span(2, 5),
        last=pick("rest", "dig", "explore", "hide"))                               # rule 7: now and then
    add("content", 24)
    add_pair("hatch5", "adult5", 16, {"stage": "hatch"}, {"stage": "adult"},       # rule 8: hatchlings hide more
             stress=span(4, 6), bold=span(2, 5), trust=span(2, 6), bored=span(2, 5))
    add_pair("shedsoon", "shedno", 16, {"shed": "soon"}, {"shed": "no"},           # rule 3: soon hides more
             hunger=span(3, 6), prey=pick("none", "far 3", "mid 2", "mid 11"), bored=span(2, 5))
    add("satedprey", 12, hunger=span(0, 2), prey=near)                             # rule 1: a snap allowed
    # A10 (appended, so every case above keeps its states): bored 0-1 with a `last`
    # that still makes sense - rest when a little tired, investigate with something
    # to look at (the other near and social 5+, or a hide near), explore, dig
    for _ in range(count(24)):
        f = fields_l1(rng, bored=span(0, 1))
        f["last"] = rng.choice(L1_PASTIMES)
        if f["last"] == "rest":
            f["energy"] = rng.randint(3, 5)
        elif f["last"] == "investigate":
            if rng.random() < 0.5:
                f["other"], f["social"] = f"near {rng.randint(1, 12)}", rng.randint(5, 8)
            else:
                f["shelter"] = f"near {rng.randint(1, 11)}"
        P.append(("bored0", line_l1(f)))
    return P


def report_l1(results):
    """results: [(case, state, goal or None)]; rows whose cases did not run print n/a"""
    by = {}
    for case, st, g in results:
        if g is not None:
            by.setdefault(case, []).append((st, g))

    def rate(case, pred):
        rows = by.get(case, [])
        return (sum(1 for st, g in rows if pred(st, g)) / len(rows) if rows else float("nan")), len(rows)

    def diff(case_a, case_b, pred):
        (a, na), (b, nb) = rate(case_a, pred), rate(case_b, pred)
        return a - b, min(na, nb)

    def last(st):
        return st.split(" last ")[1].split()[0]

    def top(case, k=3):
        d = {}
        for _, g in by.get(case, []):
            d[g] = d.get(g, 0) + 1
        n = sum(d.values()) or 1
        return ", ".join(f"{g} {c / n:.2f}" for g, c in sorted(d.items(), key=lambda kv: -kv[1])[:k])

    def goal_is(*goals):
        return lambda st, g: g in goals
    checks = [
        ("A2  P(hunt | hunger 9, prey near/mid, heat 3-7)", *rate("starving", goal_is("hunt")), ">= 0.86", lambda v: v >= 0.86),
        ("A3  P(bask | active, hunger 9, prey none, heat 0-2)", *rate("starvecold", goal_is("bask")), ">= 0.60", lambda v: v >= 0.60),
        ("A4  P(bask | active, heat 0-2, hunger <= 5, stress <= 4)", *rate("cold", goal_is("bask")), ">= 0.80", lambda v: v >= 0.80),
        ("A5  P(hide | heat 9)", *rate("hot", goal_is("hide")), ">= 0.80", lambda v: v >= 0.80),
        ("A6  P(hide | shed yes, hunger <= 6)", *rate("shedyes", goal_is("hide")), ">= 0.60", lambda v: v >= 0.60),
        ("A7  P(rest or hide | sleep, hunger <= 7)", *rate("sleep", goal_is("rest", "hide")), ">= 0.85", lambda v: v >= 0.85),
        ("A8  P(hide | stress 8-9)", *rate("stress", goal_is("hide")), ">= 0.70", lambda v: v >= 0.70),
        ("A9  P(goal != last | active, bored 8-9)", *rate("bored9", lambda s, g: g != last(s)), ">= 0.60", lambda v: v >= 0.60),
        ("A10 P(last kept | active, bored 0-1, last still fits)", *rate("bored0", lambda s, g: g == last(s)), ">= 0.40", lambda v: v >= 0.40),
        ("    P(last kept | active, bored 0-1, any last) (report)", *rate("bored0any", lambda s, g: g == last(s)), "-", lambda v: True),
        ("A11 P(explore+investigate) bold 9 - bold 0 (paired)", *diff("bold9", "bold0", goal_is("explore", "investigate")),
         ">= 0.15", lambda v: v >= 0.15),
        ("A12 P(investigate | social 0, other near)", *rate("social0", goal_is("investigate")), "<= 0.05", lambda v: v <= 0.05),
        ("r7  P(investigate | social 7-9, other near)", *rate("social9near", goal_is("investigate")),
         ">= 0.10 (now and then)", lambda v: v >= 0.10),
        ("r8  P(hide) hatchling - adult, stress 4-6 (paired)", *diff("hatch5", "adult5", goal_is("hide")), "> 0", lambda v: v > 0),
        ("r3  P(hide) shed soon - shed no (paired)", *diff("shedsoon", "shedno", goal_is("hide")), "> 0", lambda v: v > 0),
    ]
    for name, v, n, want, ok in checks:
        if n:
            val = f"{v:+.2f}" if "(paired)" in name else f"{v:5.2f}"
            print(f"  {'OK ' if ok(v) else 'BAD'}  {name:55s} = {val}  (n={n}, want {want})")
    d = {}
    for _, g in by.get("content", []):
        d[g] = d.get(g, 0) + 1
    if d:
        mx = max(d.values()) / sum(d.values())
        print(f"  {'OK ' if mx < 0.45 else 'BAD'}  {'max single goal | content':55s} = {mx:.2f}  (want < 0.45)   [{top('content')}]")
    v, n = rate("satedprey", goal_is("hunt"))
    if n:
        print(f"       {'P(hunt | hunger 0-2, prey near) (a snap is allowed)':55s} = {v:.2f}  (n={n})")
    print("\n  per case:")
    for case in ("starving", "starvecold", "cold", "hot", "shedyes", "sleep", "stress", "bored9", "bored0", "bored0any",
                 "bold9", "bold0", "social0", "social9near", "content", "hatch5", "adult5", "shedsoon", "shedno",
                 "satedprey"):
        if case in by:
            print(f"    {case:11s} {top(case, 4)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schema", type=gt.parse_schema, choices=(2, 3, 4, "L1"), default=3)
    ap.add_argument("--api", choices=("ollama", "openai", "rules"), default="ollama",
                    help=f"teacher API (openai: the lab, key from ${gt.LAB_KEY_ENV}; rules: L1's rule stand-in, no network)")
    ap.add_argument("--host", default=None, help="teacher base URL (default per --api, as gen_traces)")
    ap.add_argument("--model", default=None, help="teacher model (default per --api, as gen_traces)")
    ap.add_argument("--prompt-file", default=None, help="candidate system prompt (default: gen_traces' prompt for the schema)")
    ap.add_argument("--n", type=int, default=40, help="calls per major case (default 40 -> ~320 calls; L1 ~410)")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--cases", default=None,
                    help="L1: comma-separated cases to run (a pair's partner comes along), e.g. bold9,social9near")
    ap.add_argument("--max-requests", type=int, default=500, help="refuse a panel larger than this")
    ap.add_argument("--out", default=None, help="write every (case, state, goal) and the prompt to this JSON file")
    args = ap.parse_args()
    gt.SCHEMA = args.schema
    l1 = gt.is_l1()
    if args.api == "rules" and not l1:
        sys.exit("--api rules is the L1 rule stand-in")
    host, model = gt.teacher_defaults(args.api, args.host, args.model)
    if args.prompt_file:
        txt = open(args.prompt_file).read().strip()
        gt.SYSTEM_PROMPT = txt
        gt.SYSTEM_PROMPT_V3 = txt
        gt.SYSTEM_PROMPT_V4 = txt
        gt.SYSTEM_PROMPT_L1 = txt
    rng = random.Random(args.seed)
    P = panel_l1(rng, args.n) if l1 else panel(rng, args.schema, args.n)
    if args.cases:          # the full panel is drawn first, so a subset asks the very same states
        want = set(args.cases.split(","))
        want |= {b for a, b in L1_PAIRS.items() if a in want} | {a for a, b in L1_PAIRS.items() if b in want}
        P = [p for p in P if p[0] in want]
    if len(P) > args.max_requests:
        sys.exit(f"{len(P)} calls > --max-requests {args.max_requests}")
    prompt = gt.system_prompt()
    print(f"{len(P)} calls to {model or 'rules'} @ {host or 'local'} ({args.workers} workers); "
          f"prompt sha1 {hashlib.sha1(prompt.encode()).hexdigest()[:10]} ...", flush=True)

    def ask(item):
        case, st = item
        try:
            g = gt.ask_teacher(args.api, host, model, st, 30)
        except Exception as e:  # noqa: BLE001
            g = None
        return case, st, (g.split()[0] if g else None)

    results = {}
    rows = []
    fails = 0
    with cf.ThreadPoolExecutor(args.workers) as ex:
        for case, st, goal in ex.map(ask, P):
            rows.append((case, st, goal))
            if goal is None:
                fails += 1
                continue
            results.setdefault(case, {}).setdefault(goal, 0)
            results[case][goal] += 1

    print(f"\nfailures: {fails}")
    if args.out:
        with open(args.out, "w") as fh:
            json.dump({"schema": str(args.schema), "model": model, "host": host, "n": args.n, "seed": args.seed,
                       "prompt_sha1": hashlib.sha1(prompt.encode()).hexdigest(), "prompt": prompt,
                       "rows": [{"case": c, "state": s, "goal": g} for c, s, g in rows]}, fh, indent=1)
        print(f"-> {args.out}")
    if l1:
        report_l1(rows)
        return

    def rate(case, goal):
        d = results.get(case, {}); n = sum(d.values())
        return (d.get(goal, 0) / n if n else float("nan")), n

    def top(case):
        d = results.get(case, {}); n = sum(d.values()) or 1
        return ", ".join(f"{g} {c/n:.2f}" for g, c in sorted(d.items(), key=lambda kv: -kv[1])[:3])

    checks = [
        ("P(follow | social 9)", *rate("social9", "follow_friend"), ">= 0.40", lambda v: v >= 0.40),
        ("P(follow | social 1)", *rate("social1", "follow_friend"), "<= 0.05", lambda v: v <= 0.05),
        ("P(dart | bold 9, energetic)", *rate("bold9", "dart_play"), ">= 0.10", lambda v: v >= 0.10),
        ("P(dart | bold 1, energetic)", *rate("bold1", "dart_play"), "<= 0.03", lambda v: v <= 0.03),
        ("P(seek_food | starving)", *rate("starving", "seek_food"), ">= 0.95", lambda v: v >= 0.95),
        ("P(seek_food | lonely, not hungry)", *rate("lonely", "seek_food"), "<= 0.05", lambda v: v <= 0.05),
    ]
    if args.schema < 4:
        checks += [
            ("P(flee | shadow near, calm)", *rate("shadowcalm", "flee_shadow"), ">= 0.80", lambda v: v >= 0.80),
            ("P(flee | lonely, no shadow)", *rate("lonely", "flee_shadow"), "== 0", lambda v: v <= 0.02),
        ]
    else:
        checks += [
            ("P(bubbles again | bored 9, last bubbles)", *rate("bored9bub", "visit_bubbles"), "<= 0.15", lambda v: v <= 0.15),
            ("P(explore | bored 9, last bubbles)", *rate("bored9bub", "explore"), ">= 0.25", lambda v: v >= 0.25),
            ("P(follow again | bored 9, last follow)", *rate("bored9fol", "follow_friend"), "<= 0.15", lambda v: v <= 0.15),
            ("P(bubbles kept | bored 0, last bubbles)", *rate("bored0bub", "visit_bubbles"), ">= 0.40", lambda v: v >= 0.40),
            ("P(rest | night, bored 9, last rest)", *rate("nightbored", "rest"), ">= 0.70", lambda v: v >= 0.70),
            ("P(flee | any v4 case)", (sum(d.get("flee_shadow", 0) for d in results.values()) / max(1, sum(sum(d.values()) for d in results.values()))), sum(sum(d.values()) for d in results.values()), "== 0", lambda v: v <= 0.001),
        ]
    for name, v, n, want, ok in checks:
        print(f"  {'OK ' if ok(v) else 'BAD'}  {name:36s} = {v:.2f}  (n={n}, want {want})")
    # attractor check: no single goal should own the content fish
    d = results.get("content", {}); n = sum(d.values()) or 1
    mx = max(d.values()) / n if d else 0
    print(f"  {'OK ' if mx < 0.45 else 'BAD'}  {'max single goal | content':36s} = {mx:.2f}  (want < 0.45)   [{top('content')}]")
    if args.schema >= 3:
        print(f"       trust 9: [{top('trust9')}]\n       trust 0: [{top('trust0')}]")
    print("\n  per case:")
    for case in ("social9", "social1", "bold9", "bold1", "content", "shadowcalm", "lonely", "starving", "trust9", "trust0",
                 "bored9bub", "bored9fol", "bored0bub", "nightbored"):
        if case in results:
            print(f"    {case:10s} {top(case)}")


if __name__ == "__main__":
    main()

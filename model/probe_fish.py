#!/usr/bin/env python3
"""Probe a trained fish (schema v4) student on prompt_check.py's v4 rows F1-F13: the trained-model
side of the gate, for the aquarium (probe_l1.py is the lizard's; prompt_check.py is the teacher's).

  cd model
  POCKET_SCHEMA=4 .venv/bin/python probe_fish.py --ckpt out/ckpt_v4m.pt     # the table, out/probe_fish_ckpt_v4m.json
  POCKET_SCHEMA=4 python3 probe_fish.py --ckpt ... --device cuda            # on a GPU (harness.py's jobs)
  python3 probe_fish.py --selftest                                          # the arithmetic on a fake model (no torch)

The states are prompt_check.py's v4 panel (panel(rng, 4, --n): n 40 is the teacher's 450 lines) and
the rows are prompt_check's own table (v4_table), so the teacher's numbers and the student's are read
on the same lines by the same code. Each line's decision is read as the device reads it: BOS, the
line's words and `->`, then a softmax over the goal tokens only - the fish's eight, flee_shadow
included: the v4 vocabulary keeps that token though no label uses it, and advisor_core.c takes every
goal word it finds - at temperature 1.0. A row's value is the EXPECTED rate of the device's sampled
decisions. F12 (flee anywhere, <= 0.001) is the student's leak of probability onto the unused token;
harness.py reads it as a pass/fail row without a margin. F3 (dart | bold 9) is a report row:
chat-teacher itself gives 0.05 on it.

The JSON has probe_l1's shape (rows: row, value, n, threshold, result; passed) so harness.py reads it
unchanged. Exit status 0 unless --strict and a row fails.
"""
import argparse
import json
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_traces as gt  # noqa: E402
import probe_l1  # noqa: E402
import prompt_check as pc  # noqa: E402
import train_tokenizer as tok  # noqa: E402

SCHEMA = 4


def goals():
    """the device's goal tokens for the fish: gt.GOALS, all eight (not gt.goals(), which is the seven the labels use)"""
    return list(gt.GOALS)


def probe(dists, n, seed):
    """the v4 panel through `dists` (lines -> [(distribution, goal mass)]): prompt_check's rows;
    returns (rows, by, goal masses)"""
    gt.SCHEMA = SCHEMA
    panel = pc.panel(random.Random(seed), SCHEMA, n)
    by, masses = {}, []
    for (case, st), (d, m) in zip(panel, dists([st for _, st in panel])):
        by.setdefault(case, []).append((st, d))
        masses.append(m)
    return pc.v4_table(by), by, masses


def label(row):
    return row.split()[0]


def print_table(rows):
    print(f"  {'row':54s} {'value':>6s}  {'threshold':10s} result")
    for name, v, n, want, ok in rows:
        result = "report" if ok is None else "PASS" if ok else "FAIL"
        print(f"  {name.strip():54s} {v:6.2f}  {want:10s} {result:6s} (n={n})")


def report(rows, by, masses, extra):
    """the JSON the table came from"""
    return dict(extra, rows=[{"row": name.strip(), "value": v, "n": n, "threshold": want,
                              "result": "report" if ok is None else "PASS" if ok else "FAIL"}
                             for name, v, n, want, ok in rows],
                goal_mass={"mean": sum(masses) / len(masses), "min": min(masses)},
                cases={c: {"n": len(by[c]), "mean": pc.l1_mean_dist(by, c)} for c in pc.V4_CASE_ORDER if c in by},
                passed=all(ok for *_, ok in rows if ok is not None))


def selftest():
    """the arithmetic on a hand-made fake model: known distributions per case give known rows"""
    gs = goals()

    def mk(**kw):
        kw = dict(kw, flee_shadow=0.0)        # the unused token: a trained student gives it ~nothing
        rest = (1.0 - sum(kw.values())) / (len(gs) - len(kw))
        return {g: kw.get(g, rest) for g in gs}
    fake = {"social9": mk(follow_friend=0.8), "social1": mk(follow_friend=0.02), "bold9": mk(dart_play=0.04),
            "bold1": mk(dart_play=0.05),                                  # over F4's 0.03: the FAIL path
            "content": mk(explore=0.44, rest=0.2), "lonely": mk(seek_food=0.02), "starving": mk(seek_food=0.97),
            "trust9": mk(rest=0.3), "trust0": mk(explore=0.3),
            "bored9bub": mk(visit_bubbles=0.1, explore=0.5), "bored9fol": mk(follow_friend=0.3),   # F9 0.3 > 0.15: FAIL
            "bored0bub": mk(visit_bubbles=0.6), "nightbored": mk(rest=0.9)}
    gt.SCHEMA = SCHEMA
    panel = pc.panel(random.Random(11), SCHEMA, 40)
    case_of = {}
    for case, st in panel:
        case_of.setdefault(st, case)
    rows, by, masses = probe(lambda lines: [(fake[case_of[line]], 1.0) for line in lines], 40, 11)
    want = {"F1": (0.8, True), "F2": (0.02, True), "F3": (0.04, None), "F4": (0.05, False), "F5": (0.97, True),
            "F6": (0.02, True), "F7": (0.1, True), "F8": (0.5, True), "F9": (0.3, False), "F10": (0.6, True),
            "F11": (0.9, True), "F12": (0.0, True), "F13": (0.44, True)}
    seen = set()
    for name, v, n, w, ok in rows:
        k = label(name)
        assert k in want and k not in seen, (k, name)
        seen.add(k)
        assert abs(v - want[k][0]) < 1e-9 and ok == want[k][1], (k, v, ok, want[k])
    assert seen == set(want) and masses == [1.0] * len(panel) and len(gs) == 8
    leak = pc.v4_table({"content": [("s", dict(mk(explore=0.4), flee_shadow=0.01))]})   # a leak onto flee_shadow fails F12
    assert [r[4] for r in leak if label(r[0]) == "F12"] == [False]
    # the teacher's weights (one goal, weight 1) read through the same table
    teacher = {c: [(st, {"follow_friend": 1.0}) for st in ["s"] * 4] for c in ("social9", "social1")}
    t = {label(r[0]): r for r in pc.v4_table(teacher)}
    assert t["F1"][1] == 1.0 and t["F1"][4] and t["F2"][1] == 1.0 and not t["F2"][4]
    assert "F12" in t and t["F12"][1] == 0.0 and t["F12"][4]                # flee=True: the teacher's row
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "probe.json")
        with open(path, "w") as fh:
            json.dump(report(rows, by, masses, {"ckpt": "fake"}), fh)
        back = json.load(open(path))
    assert back["passed"] is False and [r["result"] for r in back["rows"]].count("FAIL") == 2
    assert [r["threshold"] for r in back["rows"] if r["row"].startswith("F3")] == ["-"]
    print_table(rows)
    print(f"\nselftest OK: {len(rows)} rows on {len(panel)} states match the hand-made distributions "
          f"(F4 0.05 and F9 0.30 FAIL as built, F3 report-only, F12 0.00)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ckpt", help="train.py's checkpoint (POCKET_SCHEMA=4), e.g. out/ckpt_v4m.pt")
    ap.add_argument("--n", type=int, default=40, help="prompt_check's --n: states per case (40: the teacher's 450)")
    ap.add_argument("--seed", type=int, default=11, help="prompt_check's panel seed")
    ap.add_argument("--temp", type=float, default=1.0, help="sampling temperature (the device: 1.0)")
    ap.add_argument("--out", default=None, help="the JSON (default out/probe_fish_<checkpoint name>.json)")
    ap.add_argument("--strict", action="store_true", help="exit 1 if a row fails")
    ap.add_argument("--device", default="cpu", help="cpu|cuda: where the checkpoint runs (the numbers are the same)")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on a fake model and stop")
    args = ap.parse_args()
    tok.set_schema(SCHEMA)
    if args.selftest:
        selftest()
        return
    if not args.ckpt:
        ap.error("--ckpt is required (or --selftest)")
    if args.temp <= 0:
        ap.error("--temp must be > 0 (the device samples at 1.0)")
    student = probe_l1.Student(args.ckpt, args.temp, args.device, goals=goals(), name="fish v4")
    rows, by, masses = probe(student.dists, args.n, args.seed)
    print(f"{args.ckpt}: iter {student.info['iter']}, val_loss {student.info['val_loss']}; "
          f"{sum(len(v) for v in by.values())} states (prompt_check v4 panel, --n {args.n}, seed {args.seed}), "
          f"temperature {args.temp}; goal-token mass mean {sum(masses) / len(masses):.3f}, min {min(masses):.3f}\n")
    print_table(rows)
    print("\n  per case (mean distribution, top 3):")
    for case in pc.V4_CASE_ORDER:
        if case in by:
            d = pc.l1_mean_dist(by, case)
            print(f"    {case:10s} " + ", ".join(f"{g} {p:.2f}" for g, p in sorted(d.items(), key=lambda kv: -kv[1])[:3]))
    out = args.out or os.path.join(HERE, "out", f"probe_fish_{os.path.splitext(os.path.basename(args.ckpt))[0]}.json")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    rep = report(rows, by, masses, {"ckpt": args.ckpt, "checkpoint": student.info, "n": args.n, "seed": args.seed,
                                    "temperature": args.temp})
    with open(out, "w") as fh:
        json.dump(rep, fh, indent=1)
    failed = [label(r["row"]) for r in rep["rows"] if r["result"] == "FAIL"]
    print(f"\n{'all rows PASS' if rep['passed'] else 'FAIL: ' + ', '.join(failed)}\n-> {out}")
    if args.strict and not rep["passed"]:
        sys.exit(1)


if __name__ == "__main__":
    main()

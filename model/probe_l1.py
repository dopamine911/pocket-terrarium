#!/usr/bin/env python3
"""Probe a trained L1 student on the acceptance rows A2-A12 of model/schema-lizard.md and the
rule 3/7/8 tendencies: the trained-model side of the gate (prompt_check.py is the teacher's).

  cd model
  .venv/bin/python probe_l1.py --ckpt out/ckpt_l1.pt              # the table, and out/probe_l1_ckpt_l1.json
  .venv/bin/python probe_l1.py --ckpt out/ckpt_l1.pt --n 200      # five times the states (same builders)
  python3 probe_l1.py --ckpt ... --device cuda                      # on a GPU (harness.py's jobs): rows agree with the CPU to ~1e-7
  python3 probe_l1.py --selftest                                   # the arithmetic on a fake model (no torch)

The states are prompt_check.py's L1 panel (panel_l1 with its seed 11 and --n 40: the same 464
lines the teacher was checked on) and the rows are prompt_check's own table (l1_table), so the
teacher's numbers and the student's are read on the same lines by the same code. Each line's
decision is read as the device reads it (common/llm/advisor_core.c advisor_core_infer ->
q4_model_decide_follow): BOS, the line's words and `->`, then a softmax over the seven goal tokens
only, at temperature 1.0 - so a row's value is the EXPECTED rate of the device's sampled
decisions, without sampling noise. A11 also gets its whole sweep: the bold-9 lines at every bold
0..9, and the least-squares slope of P(explore + investigate) over it (want > 0).

The checkpoint is train.py's .pt (llama2.c's Transformer, trained with POCKET_SCHEMA=L1): no C
build. Exit status 0 unless --strict and a row fails.
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
import prompt_check as pc  # noqa: E402
import train_tokenizer as tok  # noqa: E402

BATCH = 256


class Student:
    """train.py's checkpoint as the device runs it: the goal distribution after `<state> ->`.
    goals/name: the schema's goal words (default the lizard's L1; probe_fish.py passes the fish's)"""

    def __init__(self, ckpt, temp=1.0, device="cpu", goals=None, name="L1"):
        import torch
        sys.path.insert(0, os.path.join(HERE, "llama2.c"))
        from model import ModelArgs, Transformer
        self.torch, self.temp = torch, temp
        self.goals, self.name = list(goals or gt.GOALS_L1), name
        d = torch.load(ckpt, map_location="cpu")
        args = ModelArgs(**d["model_args"])
        if args.vocab_size != tok.VOCAB_SIZE:
            sys.exit(f"{ckpt}: vocab {args.vocab_size}, the {name} lexicon has {tok.VOCAB_SIZE}: not a {name} checkpoint")
        self.model = Transformer(args)
        state = {k[len("_orig_mod."):] if k.startswith("_orig_mod.") else k: v for k, v in d["model"].items()}
        self.model.load_state_dict(state, strict=True)
        self.model.to(device).eval()
        self.device = device
        self.max_seq_len = args.max_seq_len
        self.gid = [tok.encode(g)[0] for g in self.goals]
        self.info = {"iter": d.get("iter"), "val_loss": d.get("val_loss"), "model_args": d["model_args"]}

    def dists(self, lines):
        """[({goal: p}, goal-token mass)] for each line; lines of one token length go as one batch"""
        torch = self.torch
        ids = [tok.encode(line + " ->", bos=True) for line in lines]
        by_len = {}
        for i, s in enumerate(ids):
            if tok.UNK_ID in s or len(s) > self.max_seq_len:
                sys.exit(f"not a {self.name} line the model can read: {lines[i]!r}")
            by_len.setdefault(len(s), []).append(i)
        out = [None] * len(lines)
        with torch.no_grad():
            for idx in by_len.values():
                for k in range(0, len(idx), BATCH):
                    chunk = idx[k:k + BATCH]
                    logits = self.model(torch.tensor([ids[i] for i in chunk], dtype=torch.long, device=self.device))[:, -1, :]
                    p = torch.softmax(logits[:, self.gid] / self.temp, dim=-1).cpu()        # the goals only, as on the device
                    mass = torch.softmax(logits, dim=-1)[:, self.gid].sum(dim=-1).cpu()  # sanity: ~1 for a trained model
                    for j, i in enumerate(chunk):
                        out[i] = ({g: float(p[j, n]) for n, g in enumerate(self.goals)}, float(mass[j]))
        return out


def slope(ys):
    xs = range(len(ys))
    mx, my = sum(xs) / len(ys), sum(ys) / len(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


def probe(dists, n, seed):
    """the L1 panel through `dists` (lines -> [(distribution, goal mass)]): prompt_check's rows,
    plus A11's sweep; returns (rows, by, sweep curve, goal masses)"""
    gt.SCHEMA = "L1"
    panel = pc.panel_l1(random.Random(seed), n)
    by, masses = {}, []
    for (case, st), (d, m) in zip(panel, dists([st for _, st in panel])):
        by.setdefault(case, []).append((st, d))
        masses.append(m)
    rows = pc.l1_table(by)
    bases = [st for case, st in panel if case == "bold9"]
    sweep = []
    for b in range(10):
        lines = [st.replace(" bold 9 ", f" bold {b} ", 1) for st in bases]
        assert all(not gt.check_l1_line(line) for line in lines)
        part = dists(lines)
        sweep.append(sum(d["explore"] + d["investigate"] for d, _ in part) / len(part))
    s = slope(sweep)
    rows.append(("A11 slope of P(explore+investigate), bold 0..9", s, len(bases), "> 0", s > 0))
    return rows, by, sweep, masses


def show(name, v):
    """a difference or a slope signed to 3 places (a student's +0.003 is not +0.00), a rate to 2"""
    return f"{v:+.3f}" if "slope" in name or "(paired)" in name else pc.l1_value(name, v)


def label(row):
    return "content" if row.startswith("max single goal") else row.split()[0]


def print_table(rows):
    print(f"  {'row':60s} {'value':>7s}  {'threshold':22s} result")
    for name, v, n, want, ok in rows:
        result = "report" if ok is None else "PASS" if ok else "FAIL"
        print(f"  {name.strip():60s} {show(name, v):>7s}  {want:22s} {result:6s} (n={n})")


def report(rows, by, sweep, masses, extra):
    """the JSON the table came from"""
    return dict(extra, rows=[{"row": name.strip(), "value": v, "n": n, "threshold": want,
                              "result": "report" if ok is None else "PASS" if ok else "FAIL"}
                             for name, v, n, want, ok in rows],
                a11_sweep=sweep, goal_mass={"mean": sum(masses) / len(masses), "min": min(masses)},
                cases={c: {"n": len(by[c]), "mean": pc.l1_mean_dist(by, c)} for c in pc.L1_CASE_ORDER if c in by},
                passed=all(ok for *_, ok in rows if ok is not None))


def selftest():
    """the arithmetic on a hand-made fake model: known distributions per case give known rows"""
    goals = gt.GOALS_L1

    def mk(**kw):
        rest = (1.0 - sum(kw.values())) / (len(goals) - len(kw))
        return {g: kw.get(g, rest) for g in goals}
    fake = {"starving": mk(hunt=0.9), "starvecold": mk(bask=0.7), "cold": mk(bask=0.85), "hot": mk(hide=0.95),
            "shedyes": mk(hide=0.55),                                   # under A6's 0.60: the FAIL path
            "sleep": mk(rest=0.5, hide=0.4), "stress": mk(hide=0.75), "bored9": mk(explore=0.4, dig=0.3),
            "bored0": mk(explore=0.25, investigate=0.25, dig=0.25, rest=0.2), "bored0any": mk(explore=0.3, rest=0.3),
            "social0": mk(investigate=0.02, hide=0.6), "social9near": mk(investigate=0.3),
            "content": mk(explore=0.44, rest=0.2), "hatch5": mk(hide=0.6), "adult5": mk(hide=0.4),
            "elder4": mk(rest=0.5), "adult4": mk(rest=0.3), "shedsoon": mk(hide=0.7), "shedno": mk(hide=0.2),
            "satedprey": mk(hunt=0.6)}
    gt.SCHEMA = "L1"
    panel = pc.panel_l1(random.Random(11), 40)
    case_of = {}
    for case, st in panel:
        case_of.setdefault(st, case)

    def fake_dists(lines):
        out = []
        for line in lines:
            case = case_of.get(line)
            if case in (None, "bold9", "bold0"):        # the bold pair and A11's sweep: roaming grows with bold
                b = gt.parse_l1_line(line)["bold"]
                out.append((mk(explore=0.05 + 0.05 * b, investigate=0.1), 1.0))
            else:
                out.append((fake[case], 1.0))
        return out
    rows, by, sweep, masses = probe(fake_dists, 40, 11)

    def mean(case, f):
        sts = [st for c, st in panel if c == case]
        return sum(f(st) for st in sts) / len(sts)
    want = {"A2": (0.9, True), "A3": (0.7, True), "A4": (0.85, True), "A5": (0.95, True), "A6": (0.55, False),
            "A7": (0.9, True), "A8": (0.75, True),
            "A9": (mean("bored9", lambda st: 1 - fake["bored9"][pc.l1_last(st)]), True),
            # (a distribution that ignores `last` keeps it ~1/4 of the time: A10 FAILs, the second FAIL path)
            "A10": (mean("bored0", lambda st: fake["bored0"][pc.l1_last(st)]), False),
            "any last": (mean("bored0any", lambda st: fake["bored0any"][pc.l1_last(st)]), None),
            "A11": (0.45, True), "A11 slope": (0.05, True), "A12": (0.02, True), "r7": (0.3, True),
            "r8 hatch": (0.2, True), "r8 elder": (0.2, True), "r3": (0.5, True), "content": (0.44, True),
            "sated": (0.6, None)}

    def key(name):
        name = name.strip()
        for k, prefix in (("A11 slope", "A11 slope"), ("any last", "P(last kept"), ("r8 hatch", "r8  P(hide)"),
                          ("r8 elder", "r8  P(rest)"), ("content", "max single goal"), ("sated", "P(hunt")):
            if name.startswith(prefix):
                return k
        return name.split()[0]
    seen = set()
    for name, v, n, w, ok in rows:
        k = key(name)
        assert k in want and k not in seen, (k, name)
        seen.add(k)
        assert abs(v - want[k][0]) < 1e-9 and ok == want[k][1], (k, v, ok, want[k])
    assert seen == set(want), set(want) - seen
    assert all(abs(y - (0.15 + 0.05 * b)) < 1e-9 for b, y in enumerate(sweep)), sweep
    assert abs((sweep[9] - sweep[0]) - 0.45) < 1e-9 and masses == [1.0] * len(panel)
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "probe.json")
        with open(path, "w") as fh:
            json.dump(report(rows, by, sweep, masses, {"ckpt": "fake"}), fh)
        back = json.load(open(path))
    assert back["passed"] is False and [r["result"] for r in back["rows"]].count("FAIL") == 2
    print_table(rows)
    print(f"\nselftest OK: {len(rows)} rows on {len(panel)} states match the hand-made distributions "
          f"(A6 0.55 and A10 FAIL as built; the A11 sweep 0.15..0.60, slope +0.050)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ckpt", help="train.py's checkpoint (POCKET_SCHEMA=L1), e.g. out/ckpt_l1.pt")
    ap.add_argument("--n", type=int, default=40, help="prompt_check's --n: states per case (40: the teacher's 464)")
    ap.add_argument("--seed", type=int, default=11, help="prompt_check's panel seed")
    ap.add_argument("--temp", type=float, default=1.0, help="sampling temperature (the device: 1.0)")
    ap.add_argument("--out", default=None, help="the JSON (default out/probe_l1_<checkpoint name>.json)")
    ap.add_argument("--strict", action="store_true", help="exit 1 if a row fails")
    ap.add_argument("--device", default="cpu", help="cpu|cuda: where the checkpoint runs (the numbers are the same)")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on a fake model and stop")
    args = ap.parse_args()
    tok.set_schema("L1")
    if args.selftest:
        selftest()
        return
    if not args.ckpt:
        ap.error("--ckpt is required (or --selftest)")
    if args.temp <= 0:
        ap.error("--temp must be > 0 (the device samples at 1.0)")
    student = Student(args.ckpt, args.temp, args.device)
    rows, by, sweep, masses = probe(student.dists, args.n, args.seed)
    print(f"{args.ckpt}: iter {student.info['iter']}, val_loss {student.info['val_loss']}; "
          f"{sum(len(v) for v in by.values())} states (prompt_check panel, --n {args.n}, seed {args.seed}), "
          f"temperature {args.temp}; goal-token mass mean {sum(masses) / len(masses):.3f}, min {min(masses):.3f}\n")
    print_table(rows)
    print("\n  A11 sweep, P(explore+investigate) at bold 0..9: " + " ".join(f"{y:.2f}" for y in sweep))
    print("\n  per case (mean distribution):")
    for case in pc.L1_CASE_ORDER:
        if case in by:
            print(f"    {case:11s} {pc.l1_tops(by, case)}")
    out = args.out or os.path.join(HERE, "out", f"probe_l1_{os.path.splitext(os.path.basename(args.ckpt))[0]}.json")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    rep = report(rows, by, sweep, masses, {"ckpt": args.ckpt, "checkpoint": student.info, "n": args.n,
                                           "seed": args.seed, "temperature": args.temp})
    with open(out, "w") as fh:
        json.dump(rep, fh, indent=1)
    failed = [label(r["row"]) for r in rep["rows"] if r["result"] == "FAIL"]
    print(f"\n{'all rows PASS' if rep['passed'] else 'FAIL: ' + ', '.join(failed)}"
          f"\n-> {out}")
    if args.strict and not rep["passed"]:
        sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Fold raw trace files into one clean training set. Stdlib only.

  python3 fold_traces.py --schema 3 out/v3_traces_*.jsonl --out out/v3_clean.jsonl
  python3 fold_traces.py --schema L1 out/l1_traces_*.jsonl --out out/l1_clean.jsonl

- drops malformed lines and any line with a word outside the schema's closed
  vocabulary (train_tokenizer.lexicon_for); L1 also drops a state that is not
  a well-formed L1 line (field order, values: gen_traces.check_l1_line) and a
  goal that is not `<one of the seven> urgency <0-9>`
- de-duplicates exact (state, goal) pairs; for a state seen with several goals
  keeps them all (the teacher's own variance is signal - the student learns it
  as a soft distribution and the device samples from it)
- prints the goal distribution so a prompt-is-DNA skew (docs/stats.md) is
  caught before a 30-minute train
"""
import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_tokenizer as tok  # noqa: E402
import gen_traces as gt  # noqa: E402


def l1_coverage(kept):
    """the shares the L1 acceptance rows lean on, and the label each edge got"""
    rows = [(gt.parse_l1_line(o["state"]), o["goal"].split()[0]) for o in kept]
    n = len(rows)

    def line(name, pred, want=None):
        hit = [g for s, g in rows if pred(s)]
        if not hit:
            print(f"    {name:44s}      0")
            return
        tail = f"  -> {sum(1 for g in hit if g in want) / len(hit):.2f} {'|'.join(want)}" if want else ""
        print(f"    {name:44s} {len(hit):6d} ({100 * len(hit) / n:4.1f}%){tail}")
    print("  L1 coverage (state share; the teacher's rate on it):")
    line("hunger 9, prey near/mid, heat 3-7 (A2)",
         lambda s: s["hunger"] == 9 and s["prey"] and s["prey"][0] != "far" and 3 <= s["heat"] <= 7, ["hunt"])
    line("hunger 9, prey none, heat 0-2, active (A3)",
         lambda s: s["hunger"] == 9 and not s["prey"] and s["heat"] <= 2 and s["phase"] == "active", ["bask"])
    line("heat 0-2, hunger <= 5, stress <= 4, active (A4)",
         lambda s: s["heat"] <= 2 and s["hunger"] <= 5 and s["stress"] <= 4 and s["phase"] == "active", ["bask"])
    line("heat 9 (A5)", lambda s: s["heat"] == 9, ["hide"])
    line("shed yes, hunger <= 6 (A6)", lambda s: s["shed"] == "yes" and s["hunger"] <= 6, ["hide"])
    line("phase sleep, hunger <= 7 (A7)", lambda s: s["phase"] == "sleep" and s["hunger"] <= 7, ["rest", "hide"])
    line("stress 8-9 (A8)", lambda s: s["stress"] >= 8, ["hide"])
    line("social 0-2, other near (A12: investigate ~0)",
         lambda s: s["social"] <= 2 and s["other"] and s["other"][0] == "near", ["investigate"])
    line("phase settle", lambda s: s["phase"] == "settle")
    bored = [(s, g) for s, g in rows if s["bored"] >= 8 and s["phase"] == "active"]
    if bored:
        print(f"    {'bored 8-9, active (A9: a goal != last)':44s} {len(bored):6d} ({100 * len(bored) / n:4.1f}%)"
              f"  -> {sum(1 for s, g in bored if g != s['last']) / len(bored):.2f} changed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="raw jsonl files or globs")
    ap.add_argument("--schema", type=tok.parse_schema, choices=(2, 3, 4, "L1"), default=3)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    tok.set_schema(args.schema)
    words = set(tok.LEXICON)
    l1 = args.schema == "L1"

    paths = [p for g in args.inputs for p in sorted(glob.glob(g))]
    if not paths:
        sys.exit("no input files")
    seen, kept, bad_vocab, bad_json, bad_form, dups = set(), [], 0, 0, 0, 0
    goals = {}
    for path in paths:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    o = json.loads(line)
                    state, goal = o["state"], o["goal"]
                except (ValueError, KeyError, TypeError):
                    bad_json += 1
                    continue
                if any(w not in words for w in (state + " " + goal).split()):
                    bad_vocab += 1
                    continue
                if l1 and (gt.check_l1_line(state) or gt.check_l1_goal(goal)):
                    bad_form += 1
                    continue
                key = (state, goal)
                if key in seen:
                    dups += 1
                    continue
                seen.add(key)
                kept.append({"state": state, "goal": goal})
                g = goal.split()[0]
                goals[g] = goals.get(g, 0) + 1
    with open(args.out, "w") as out:
        for o in kept:
            out.write(json.dumps(o) + "\n")
    n = len(kept)
    print(f"{len(paths)} files -> {n} clean pairs -> {args.out}")
    print(f"  dropped: {bad_json} malformed, {bad_vocab} off-vocabulary, "
          + (f"{bad_form} not L1-shaped, " if l1 else "") + f"{dups} exact duplicates")
    if not n:
        return
    print("  goal distribution:")
    for g, c in sorted(goals.items(), key=lambda kv: -kv[1]):
        flag = "  <-- check the prompt (v2 incident was 45%)" if c / n > 0.40 else ""
        print(f"    {g:14s} {c:6d}  {100 * c / n:4.1f}%{flag}")
    if l1:
        l1_coverage(kept)
        return
    if args.schema >= 4:
        bored = sum(1 for o in kept if any(f" bored {d} " in o["state"] for d in "6789"))
        rut = sum(1 for o in kept if any(f" bored {d} " in o["state"] for d in "789")
                  and o["goal"].split()[0] == o["state"].split(" last ")[1].split()[0])
        print(f"  v4 coverage: bored 6-9 {bored} ({100 * bored / n:.1f}%); bored 7-9 that REPEAT last: {rut} "
              f"(the teacher should rarely do this)")
    if args.schema >= 3:
        none = sum(1 for o in kept if " friend none " in o["state"] + " ")
        calm = sum(1 for o in kept if "shadow near" in o["state"] and any(f"stress {d}" in o["state"] for d in "0123"))
        print(f"  v3 coverage: friend none {none} ({100 * none / n:.1f}%), shadow near + calm {calm}")


if __name__ == "__main__":
    main()

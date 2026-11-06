#!/usr/bin/env python3
"""Compare harness runs of different model sizes: quality, size and speed in one table.

  python3 size_report.py l1-focus2=/path/to/out/harness/l1-focus2 size-d256-n8-all ...
  python3 size_report.py --markdown out/harness/size-report.md size-d256-n8-all ...

A spec is a run name under out/harness/ or label=<run directory>. Per run: parameters, q4 size,
the q4 runner's speed on this machine (out/runq4, built by `gcc -O3 -o out/runq4 runq4.c -lm`),
the goal NLL and accuracy of each seed's kept checkpoint (train.py: the device's goal softmax on
held-out docs), how many seeds pass every probe row, the worst row-by-seed margin, and the rows
a seed fails. The pendant column scales the measured pendant speed (PLAN.md P7: 11.9 tok/s for
the 14M) by this machine's speed ratio: a projection, not a measurement - memory bandwidth
differs between an x86 core and the ESP32-S3.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys

import harness as h

HERE = os.path.dirname(os.path.abspath(__file__))
STATES = {   # a full-length prompt per schema, for timing the q4 runner
    "L1": ("zone 4 hunger 7 energy 5 stress 2 curiosity 6 heat 3 bold 4 social 2 stage adult trust 6 bored 1 "
           "shed no prey mid 2 other none warm far 11 shelter near 7 wall clear last explore phase active"),
    "4": ("zone 2 hunger 7 energy 5 stress 2 curiosity 8 bold 4 social 6 stage adult trust 5 bored 3 food near 12 "
          "friend mid 3 bubble mid 10 reef far 7 wall clear last explore time day"),
}
TOKENIZERS = {"L1": "tokenizer_lizard.bin", "4": "tokenizer.bin"}
PENDANT_TOKS = {"L1": 11.9}   # the lizard 14M measured on the pendant (PLAN.md P7); the fish 14M is not entered: measure it
STATE = STATES["L1"]


def load(spec):
    label, _, path = spec.partition("=")
    if not path:
        label, path = spec, os.path.join(h.OUT, spec)
    with open(os.path.join(path, "report.json")) as fh:
        return label, path, json.load(fh)


def toks_per_s(model_bin, runner, tok_bin, repeats=9):
    """median of the runner's own 'achieved tok/s' over a full 45-token prompt"""
    vals = []
    for _ in range(repeats):
        err = subprocess.run([runner, model_bin, "-z", tok_bin, "-t", "0", "-n", "52", "-i", STATE + " ->"],
                             capture_output=True, text=True, timeout=60).stderr
        for line in err.splitlines():
            if line.startswith("achieved tok/s:"):
                vals.append(float(line.split(":")[1]))
    return statistics.median(vals) if vals else None


def failing_rows(report, seed):
    bad = []
    for row in report["rows"].values():
        for value in row["values"].get(str(seed), []):
            m = h.margin(value, row["threshold"])
            if m is not None and m < 0:
                bad.append(row["name"].split()[0])
                break
    return bad


def summarize(label, path, report, runner, tok_bin):
    seeds = list(report["jobs"])
    jobs = report["jobs"]
    tr = {s: jobs[s]["train"] for s in seeds}
    fails = {s: failing_rows(report, s) for s in seeds}
    first = os.path.join(path, f"s{seeds[0]}", "model_q4.bin")
    return {
        "label": label, "params_m": tr[seeds[0]]["params_m"], "seeds": len(seeds),
        "q4_mb": os.path.getsize(first) / 1e6 if os.path.exists(first) else None,
        "toks": toks_per_s(first, runner, tok_bin) if os.path.exists(first) and os.path.exists(runner) else None,
        "nll": statistics.fmean(t["goal_nll"] for t in tr.values()),
        "nll_sd": statistics.stdev([t["goal_nll"] for t in tr.values()]) if len(tr) > 1 else 0.0,
        "acc": statistics.fmean(t["goal_acc"] for t in tr.values()),
        "passing": sum(not f for f in fails.values()),
        "worst_margin": min((report["per_seed"][s]["min_margin"] for s in seeds
                             if report["per_seed"][s]["min_margin"] is not None), default=None),
        "fails": sorted({r for f in fails.values() for r in f}),
        "s_per_iter": statistics.fmean(t["s_per_iter"] for t in tr.values()),
    }


def table(rows, ref, pendant):
    head = ("| run | params | q4 MB | x86 tok/s | pendant tok/s (proj.) | goal NLL (sd) | goal acc | "
            "seeds passing all rows | worst scaled margin | rows failed by some seed |")
    out = [head, "|" + "---|" * 10]
    for r in rows:
        proj = pendant * r["toks"] / ref["toks"] if pendant and r["toks"] and ref and ref["toks"] else None
        out.append(f"| {r['label']} | {r['params_m']:.2f}M | {(r['q4_mb'] or 0):.2f} | "
                   f"{'-' if r['toks'] is None else format(r['toks'], '.0f')} | "
                   f"{'-' if proj is None else format(proj, '.1f')} | "
                   f"{r['nll']:.4f} ({r['nll_sd']:.4f}) | {r['acc']:.3f} | {r['passing']}/{r['seeds']} | "
                   f"{h.fmt(r['worst_margin'], 3, sign=True)} | {', '.join(r['fails']) or '-'} |")
    return "\n".join(out)


def selftest():
    assert h.margin(0.9, ">= 0.86") > 0 and h.margin(0.8, ">= 0.86") < 0 and h.margin(0.5, "-") is None
    assert abs(statistics.median([1, 3, 2]) - 2) < 1e-9
    rep = {"rows": {"A2": {"name": "A2  P(hunt)", "threshold": ">= 0.86", "values": {"1": [0.9, 0.8], "2": [0.9, 0.9]}}}}
    assert failing_rows(rep, 1) == ["A2"] and failing_rows(rep, 2) == []
    print("selftest OK: failing rows, medians")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("specs", nargs="*")
    ap.add_argument("--reference", help="the spec the pendant projection is scaled from (default: the first spec)")
    ap.add_argument("--schema", choices=sorted(STATES), default="L1", help="L1 (the lizard) or 4 (the fish): the timing prompt, tokenizer and pendant default")
    ap.add_argument("--pendant-toks", type=float, default=None,
                    help="measured pendant speed of the reference (default: the lizard 14M's 11.9 for L1; none for the fish, so no projection)")
    ap.add_argument("--runner", default=os.path.join(HERE, "out", "runq4"))
    ap.add_argument("--tokenizer", default=None, help="default: out/tokenizer_lizard.bin (L1) or out/tokenizer.bin (4)")
    ap.add_argument("--markdown", help="also write the table here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.specs:
        ap.error("give at least one run")
    global STATE
    STATE = STATES[args.schema]
    tokenizer = args.tokenizer or os.path.join(HERE, "out", TOKENIZERS[args.schema])
    pendant = args.pendant_toks if args.pendant_toks is not None else PENDANT_TOKS.get(args.schema)
    rows = [summarize(*load(s), args.runner, tokenizer) for s in args.specs]
    ref = next((r for r in rows if r["label"] == (args.reference or args.specs[0]).partition("=")[0]), rows[0])
    text = table(rows, ref, pendant)
    print(text)
    if args.markdown:
        with open(args.markdown, "w") as fh:
            fh.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

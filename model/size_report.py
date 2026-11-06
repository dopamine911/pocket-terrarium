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
STATE = ("zone 4 hunger 7 energy 5 stress 2 curiosity 6 heat 3 bold 4 social 2 stage adult trust 6 bored 1 "
         "shed no prey mid 2 other none warm far 11 shelter near 7 wall clear last explore phase active")


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


def missing_seeds(report):
    """the configured seeds with no collected job; None when the report predates recording the seed set"""
    if "seeds" not in report:
        return None
    return [s for s in report["seeds"] if str(s) not in report["jobs"]]


def complete(report):
    """every configured seed was collected (a report from before the seed set was recorded is not known to be)"""
    miss = missing_seeds(report)
    return miss is not None and not miss


def metric(tr, key):
    """the values of `key` across the collected jobs, or None when any job lacks it (a report from before the
    metric was measured: a mean of some seeds would pass for the run's)"""
    vals = [t.get(key) for t in tr.values()]
    return None if not vals or None in vals else vals


def summarize(label, path, report, runner, tok_bin):
    jobs = report["jobs"]
    seeds = list(jobs)
    tr = {s: jobs[s].get("train", {}) for s in seeds}
    fails = {s: failing_rows(report, s) for s in seeds}
    first = os.path.join(path, f"s{seeds[0]}", "model_q4.bin") if seeds else None
    nll, acc = metric(tr, "goal_nll"), metric(tr, "goal_acc")
    miss = missing_seeds(report)
    margins = [report["per_seed"][s]["min_margin"] for s in seeds if report.get("per_seed", {}).get(s, {}).get("min_margin") is not None]
    iters = metric(tr, "s_per_iter")
    return {
        "label": label, "params_m": tr[seeds[0]].get("params_m") if seeds else None, "seeds": len(seeds),
        "expected": len(report["seeds"]) if "seeds" in report else None, "missing": miss,
        "q4_mb": os.path.getsize(first) / 1e6 if first and os.path.exists(first) else None,
        "toks": toks_per_s(first, runner, tok_bin) if first and os.path.exists(first) and os.path.exists(runner) else None,
        "nll": statistics.fmean(nll) if nll else None,
        "nll_sd": (statistics.stdev(nll) if len(nll) > 1 else 0.0) if nll else None,
        "acc": statistics.fmean(acc) if acc else None,
        "passing": sum(not f for f in fails.values()),
        "worst_margin": min(margins, default=None),
        "fails": sorted({r for f in fails.values() for r in f}),
        "s_per_iter": statistics.fmean(iters) if iters else None,
    }


def passing_cell(r):
    """seeds passing every row, out of the CONFIGURED seeds: a seed with no collected job is not a pass, and the cell says so"""
    if r["expected"] is None:
        return f"{r['passing']}/{r['seeds']} (seed set not recorded)"
    if r["missing"]:
        return f"{r['passing']}/{r['expected']} **INCOMPLETE: seed{'s' if len(r['missing']) > 1 else ''} {', '.join(map(str, r['missing']))} not collected**"
    return f"{r['passing']}/{r['expected']}"


def table(rows, ref, pendant):
    """ref: the row the pendant projection scales from, or None (no projection: the pendant speed belongs to one model)"""
    head = ("| run | params | q4 MB | x86 tok/s | pendant tok/s (proj.) | goal NLL (sd) | goal acc | "
            "seeds passing all rows | worst scaled margin | rows failed by some seed |")
    out = [head, "|" + "---|" * 10]
    for r in rows:
        proj = pendant * r["toks"] / ref["toks"] if r["toks"] and ref and ref["toks"] else None
        cells = [r["label"],
                 f"{r['params_m']:.2f}M" if r["params_m"] is not None else "-",
                 f"{r['q4_mb']:.2f}" if r["q4_mb"] is not None else "-",
                 f"{r['toks']:.0f}" if r["toks"] else "-",
                 f"{proj:.1f}" if proj else "-",
                 f"{r['nll']:.4f} ({r['nll_sd']:.4f})" if r["nll"] is not None else "-",
                 f"{r['acc']:.3f}" if r["acc"] is not None else "-",
                 passing_cell(r),
                 h.fmt(r["worst_margin"], 3, sign=True),
                 ", ".join(r["fails"]) or "-"]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def pick_reference(rows, name):
    """the row the projection scales from. A reference that was asked for and is not among the runs is an error: falling
    back to another model would calibrate the pendant speed (measured for the 14M) to the wrong one"""
    for r in rows:
        if r["label"] == name:
            return r
    raise ValueError(f"the reference {name!r} is not among the runs ({', '.join(r['label'] for r in rows)}); "
                     "its report is missing - run it, or leave --reference out to skip the pendant projection")


def selftest():
    assert h.margin(0.9, ">= 0.86") > 0 and h.margin(0.8, ">= 0.86") < 0 and h.margin(0.5, "-") is None
    assert abs(statistics.median([1, 3, 2]) - 2) < 1e-9
    rep = {"rows": {"A2": {"name": "A2  P(hunt)", "threshold": ">= 0.86", "values": {"1": [0.9, 0.8], "2": [0.9, 0.9]}}}}
    assert failing_rows(rep, 1) == ["A2"] and failing_rows(rep, 2) == []
    # a legacy report (no goal metrics, no seed set), a partial one and a complete one, the way harness.py writes them
    def job(**train):
        return {"train": {"params_m": 3.2, "s_per_iter": 0.2, **train}}
    row = {"A2": {"name": "A2  P(hunt)", "threshold": ">= 0.86", "values": {"1": [0.9], "2": [0.9], "3": [0.9]}}}
    ps = {str(i): {"min_margin": 0.04} for i in (1, 2, 3)}
    legacy = {"jobs": {"1": job(), "2": job()}, "rows": row, "per_seed": ps}
    r = summarize("old", "/nonexistent", legacy, "/nonexistent", "/nonexistent")
    assert r["nll"] is None and r["acc"] is None and r["expected"] is None
    assert "seed set not recorded" in table([r], None, 11.9) and not complete(legacy)
    partial = {"seeds": [1, 2, 3], "jobs": {"1": job(goal_nll=0.3, goal_acc=0.8)}, "rows": row, "per_seed": ps}
    r = summarize("part", "/nonexistent", partial, "/nonexistent", "/nonexistent")
    cell = passing_cell(r)
    assert r["passing"] == 1 and r["expected"] == 3 and r["missing"] == [2, 3] and not complete(partial)
    assert cell.startswith("1/3") and "INCOMPLETE" in cell and "2, 3" in cell, cell      # never "1/1"
    full = {"seeds": [1, 2], "jobs": {"1": job(goal_nll=0.3, goal_acc=0.8), "2": job(goal_nll=0.4, goal_acc=0.7)}, "rows": row, "per_seed": ps}
    r = summarize("full", "/nonexistent", full, "/nonexistent", "/nonexistent")
    assert complete(full) and passing_cell(r) == "2/2" and abs(r["nll"] - 0.35) < 1e-9
    nothing = {"seeds": [1, 2, 3], "jobs": {}, "rows": {}, "per_seed": {}}
    r = summarize("none", "/nonexistent", nothing, "/nonexistent", "/nonexistent")
    assert passing_cell(r).startswith("0/3") and "INCOMPLETE" in table([r], None, 11.9)
    # the reference: found, or a clear error - never another model's speed
    rows = [{"label": "a", "toks": 50.0}, {"label": "b", "toks": 80.0}]
    assert pick_reference(rows, "b")["toks"] == 80.0
    try:
        pick_reference(rows, "l1-focus2")
        raise AssertionError("a missing reference was accepted")
    except ValueError as e:
        assert "l1-focus2" in str(e)
    print("selftest OK: failing rows, medians, legacy / partial / empty reports, the reference")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("specs", nargs="*")
    ap.add_argument("--reference", help="the run the pendant projection is scaled from, by label: its speed here against --pendant-toks "
                    "(without it there is no projection; a reference that is not among the runs is an error)")
    ap.add_argument("--pendant-toks", type=float, default=11.9, help="measured pendant speed of the reference run (the 14M)")
    ap.add_argument("--complete", metavar="RUN", help="exit 0 if RUN's report holds every configured seed, else 1 (what the sweep script resumes by)")
    ap.add_argument("--runner", default=os.path.join(HERE, "out", "runq4"))
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "out", "tokenizer_lizard.bin"))
    ap.add_argument("--markdown", help="also write the table here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.complete:
        try:
            return 0 if complete(load(args.complete)[2]) else 1
        except FileNotFoundError:
            return 1
    if not args.specs:
        ap.error("give at least one run")
    rows = [summarize(*load(s), args.runner, args.tokenizer) for s in args.specs]
    ref = None
    if args.reference:
        try:
            ref = pick_reference(rows, args.reference)
        except ValueError as e:
            ap.error(str(e))
    text = table(rows, ref, args.pendant_toks)
    print(text)
    if args.markdown:
        with open(args.markdown, "w") as fh:
            fh.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

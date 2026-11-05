#!/usr/bin/env python3
"""Focused L1 teacher panel and append-safe top-up generator for A10/A12.

The regular trace generator samples a running terrarium.  This sampler instead
holds urgent drives quiet so it can teach the two relationships that need dense,
controlled examples:

* A10: a fresh, still-suitable pastime is kept;
* A12: matched social 0-2 / 7-9 lizards react differently to the same nearby
  lizard.

Every training label is returned by the configured teacher.  Extra provenance
fields in the JSONL are ignored by fold_traces.py but make an interrupted run
safe to resume with the same seed and prompt.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_traces as gt  # noqa: E402
import prompt_check as pc  # noqa: E402
import train_tokenizer as tok  # noqa: E402

SAMPLER = "l1-focus-v1"
A10_LAST = ("explore", "investigate", "dig", "rest")
A12_LAST = ("explore", "dig", "rest", "hide")


def _sight(rng, kinds):
    kind = rng.choice(kinds)
    return "none" if kind == "none" else f"{kind} {rng.randint(1, 12)}"


def focused_panel(rng, a10_count, a12_pairs, a12x_pairs=0):
    """Return deterministic ``(case, index, state)`` focused examples.

    A10 examples are balanced across all four pastimes.  An investigate-last
    example always has a nearby, socially tolerated lizard; other A10 examples
    have no nearby lizard, so keeping the current goal cannot conflict with
    A12.  Each A12 pair is byte-for-byte identical except for social.

    A12x pairs (2026-11-05) close the gap between the two: a lizard that has only
    just begun investigating the nearby other (last investigate, bored 0-1).  Social
    7-9 keeps it up (A10); social 0-2 turns away (A12).  The first focused model
    investigated 0.12 of those low-social states and ~0.00 of every other last,
    because neither case above ever showed one.  Drawn after the A12 pairs, so a
    seed's A10/A12 states do not move when A12x is added.
    """
    gt.SCHEMA = "L1"
    panel = []

    lasts = [A10_LAST[i % len(A10_LAST)] for i in range(a10_count)]
    rng.shuffle(lasts)
    for index, last in enumerate(lasts):
        fields = pc.fields_l1(
            rng,
            phase="active",
            bored=lambda r: r.randint(0, 1),
            hunger=lambda r: r.randint(0, 3),
            energy=lambda r: r.randint(5, 9),
            stress=lambda r: r.randint(0, 2),
            curiosity=lambda r: r.randint(3, 8),
            heat=lambda r: r.randint(5, 6),
            shed="no",
            prey="none",
            other=lambda r: _sight(r, ("none", "mid", "far")),
            last=last,
        )
        if last == "rest":
            fields["energy"] = rng.randint(3, 5)
        elif last == "investigate":
            fields["other"] = f"near {rng.randint(1, 12)}"
            fields["social"] = rng.randint(5, 9)
        panel.append(("a10", index, pc.line_l1(fields)))

    pair_lasts = [A12_LAST[i % len(A12_LAST)] for i in range(a12_pairs)]
    rng.shuffle(pair_lasts)
    for index, last in enumerate(pair_lasts):
        fields = pc.fields_l1(
            rng,
            phase="active",
            bored=lambda r: r.randint(2, 5),
            hunger=lambda r: r.randint(0, 3),
            energy=lambda r: r.randint(5, 9),
            stress=lambda r: r.randint(0, 2),
            curiosity=lambda r: r.randint(3, 8),
            heat=lambda r: r.randint(5, 6),
            shed="no",
            prey="none",
            other=lambda r: f"near {r.randint(1, 12)}",
            last=last,
        )
        low, high = rng.randint(0, 2), rng.randint(7, 9)
        panel.append(("a12_low", index, pc.line_l1(dict(fields, social=low))))
        panel.append(("a12_high", index, pc.line_l1(dict(fields, social=high))))
    for index in range(a12x_pairs):
        fields = pc.fields_l1(
            rng,
            phase="active",
            bored=lambda r: r.randint(0, 1),
            hunger=lambda r: r.randint(0, 3),
            energy=lambda r: r.randint(5, 9),
            stress=lambda r: r.randint(0, 2),
            curiosity=lambda r: r.randint(3, 8),
            heat=lambda r: r.randint(5, 6),
            shed="no",
            prey="none",
            other=lambda r: f"near {r.randint(1, 12)}",
            last="investigate",
        )
        low, high = rng.randint(0, 2), rng.randint(7, 9)
        panel.append(("a12x_low", index, pc.line_l1(dict(fields, social=low))))
        panel.append(("a12x_high", index, pc.line_l1(dict(fields, social=high))))

    validate_panel(panel, a10_count, a12_pairs, a12x_pairs)
    return panel


def validate_panel(panel, a10_count, a12_pairs, a12x_pairs=0):
    """Reject malformed/off-vocabulary states and sampler-contract drift."""
    tok.set_schema("L1")
    words = set(tok.LEXICON)
    expected = a10_count + 2 * a12_pairs + 2 * a12x_pairs
    if len(panel) != expected:
        raise ValueError(f"focused panel has {len(panel)} states, expected {expected}")
    keys = [(case, index) for case, index, _ in panel]
    if len(set(keys)) != len(keys):
        raise ValueError("focused panel has duplicate case/index keys")

    a10_lasts = []
    pairs = {}
    xpairs = {}
    for case, index, state in panel:
        problem = gt.check_l1_line(state)
        if problem:
            raise ValueError(f"{case}[{index}] is not L1-shaped: {problem}")
        unknown = [word for word in state.split() if word not in words]
        if unknown:
            raise ValueError(f"{case}[{index}] has off-vocabulary words: {unknown}")
        fields = gt.parse_l1_line(state)
        if case == "a10":
            a10_lasts.append(fields["last"])
            quiet = (fields["phase"] == "active" and fields["bored"] <= 1
                     and fields["hunger"] <= 3 and fields["stress"] <= 2
                     and 5 <= fields["heat"] <= 6 and fields["shed"] == "no"
                     and fields["prey"] is None)
            if not quiet or fields["last"] not in A10_LAST:
                raise ValueError(f"a10[{index}] is not a quiet, fresh pastime")
            if fields["last"] == "rest" and not 3 <= fields["energy"] <= 5:
                raise ValueError(f"a10[{index}] rest is not supported by middling energy")
            if fields["last"] == "investigate":
                supported = (fields["other"] is not None and fields["other"][0] == "near"
                             and fields["social"] >= 5)
                if not supported:
                    raise ValueError(f"a10[{index}] investigate lacks supporting social/other context")
            elif fields["other"] is not None and fields["other"][0] == "near":
                raise ValueError(f"a10[{index}] has a new nearby-lizard event")
        elif case in ("a12_low", "a12_high"):
            if fields["other"] is None or fields["other"][0] != "near":
                raise ValueError(f"{case}[{index}] has no nearby lizard")
            pairs.setdefault(index, {})[case] = fields
        elif case in ("a12x_low", "a12x_high"):
            fresh = (fields["phase"] == "active" and fields["bored"] <= 1 and fields["last"] == "investigate"
                     and fields["other"] is not None and fields["other"][0] == "near")
            if not fresh:
                raise ValueError(f"{case}[{index}] is not a fresh investigate with the other near")
            xpairs.setdefault(index, {})[case.replace("a12x", "a12")] = fields
        else:
            raise ValueError(f"unknown focused case {case!r}")

    counts = [a10_lasts.count(last) for last in A10_LAST]
    if counts and max(counts) - min(counts) > 1:
        raise ValueError(f"A10 last goals are not balanced: {dict(zip(A10_LAST, counts))}")
    for name, got, want in (("A12", pairs, a12_pairs), ("A12x", xpairs, a12x_pairs)):
        if len(got) != want or any(set(pair) != {"a12_low", "a12_high"} for pair in got.values()):
            raise ValueError(f"{name} pairs are incomplete")
        for index, pair in got.items():
            low, high = pair["a12_low"], pair["a12_high"]
            if not 0 <= low["social"] <= 2 or not 7 <= high["social"] <= 9:
                raise ValueError(f"{name} pair {index} has wrong social bands")
            if {k: v for k, v in low.items() if k != "social"} != {k: v for k, v in high.items() if k != "social"}:
                raise ValueError(f"{name} pair {index} differs in more than social")


def summarize(panel, results):
    """Metrics for complete or partial teacher results, including A10 per-last."""
    by_key = {(row["case"], row["index"]): row for row in results if row.get("goal")}
    states = {(case, index): gt.parse_l1_line(state) for case, index, state in panel}

    a10 = [row for key, row in by_key.items() if key[0] == "a10"]
    kept = [row["goal"].split()[0] == states[(row["case"], row["index"])]["last"] for row in a10]
    per_last = {}
    for last in A10_LAST:
        rows = [row for row in a10 if states[(row["case"], row["index"])]["last"] == last]
        per_last[last] = {
            "n": len(rows),
            "kept": (sum(row["goal"].split()[0] == last for row in rows) / len(rows) if rows else None),
        }

    def pair_rates(prefix):
        low = [row for key, row in by_key.items() if key[0] == f"{prefix}_low"]
        high = [row for key, row in by_key.items() if key[0] == f"{prefix}_high"]
        low_rate = sum(row["goal"].split()[0] == "investigate" for row in low) / len(low) if low else None
        high_rate = sum(row["goal"].split()[0] == "investigate" for row in high) / len(high) if high else None
        paired = []
        for index in range(sum(1 for case, _, _ in panel if case == f"{prefix}_low")):
            lo, hi = by_key.get((f"{prefix}_low", index)), by_key.get((f"{prefix}_high", index))
            if lo and hi:
                paired.append((lo["goal"].split()[0] == "investigate", hi["goal"].split()[0] == "investigate"))
        return {
            "pairs_answered": len(paired),
            "low_n": len(low),
            "low_investigate": low_rate,
            "high_n": len(high),
            "high_investigate": high_rate,
            "paired_delta": (high_rate - low_rate if low_rate is not None and high_rate is not None else None),
            "low_no_high_yes": (sum(not lo and hi for lo, hi in paired) / len(paired) if paired else None),
        }

    return {
        "answered": len(by_key),
        "requested": len(panel),
        "a10": {
            "n": len(a10),
            "kept": sum(kept) / len(kept) if kept else None,
            "by_last": per_last,
        },
        "a12": pair_rates("a12"),
        "a12x": pair_rates("a12x"),
    }


def print_summary(metrics):
    a10 = metrics["a10"]
    print(f"answered {metrics['answered']}/{metrics['requested']}")
    if a10["n"]:
        print(f"  A10 last kept {a10['kept']:.3f} (n={a10['n']})")
        print("      " + "  ".join(
            f"{last} {row['kept']:.3f} (n={row['n']})" for last, row in a10["by_last"].items() if row["n"]))
    for key, label in (("a12", "A12 investigate"), ("a12x", "A12x investigate, last investigate bored 0-1")):
        a12 = metrics.get(key)
        if a12 and a12["low_n"]:
            print(f"  {label}: social 0-2 {a12['low_investigate']:.3f} (n={a12['low_n']}), "
                  f"social 7-9 {a12['high_investigate']:.3f} (n={a12['high_n']}), "
                  f"paired delta {a12['paired_delta']:+.3f}")


def _read_trace_rows(path, expected, seed, prompt_sha1):
    """Load this sampler's valid rows; repair only a torn final JSONL record."""
    if not os.path.exists(path):
        return {}
    rows = {}
    size = os.path.getsize(path)
    good_end = 0
    torn = False
    with open(path, "rb") as fh:
        while True:
            start = fh.tell()
            raw = fh.readline()
            if not raw:
                break
            end = fh.tell()
            try:
                row = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                if end == size and not raw.endswith(b"\n"):
                    torn = True
                    good_end = start
                    break
                raise ValueError(f"{path}: malformed JSONL record at byte {start}: {exc}") from None
            good_end = end
            key = (row.get("case"), row.get("index"))
            if row.get("sampler") != SAMPLER or row.get("seed") != seed or row.get("prompt_sha1") != prompt_sha1:
                raise ValueError(f"{path}: row {key} belongs to another sampler, seed, or prompt")
            if key not in expected or row.get("state") != expected[key]:
                raise ValueError(f"{path}: row {key} does not match this deterministic panel")
            if gt.check_l1_goal(row.get("goal", "")):
                raise ValueError(f"{path}: row {key} has an invalid goal")
            if key in rows:
                raise ValueError(f"{path}: duplicate row {key}")
            rows[key] = row
    if torn:
        with open(path, "r+b") as fh:
            fh.truncate(good_end)
        print(f"recovered torn final record in {path}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--a10", type=int, default=160, help="number of fresh-last A10 states")
    ap.add_argument("--a12-pairs", type=int, default=160, help="matched low/high-social pairs")
    ap.add_argument("--a12x-pairs", type=int, default=0,
                    help="matched pairs of a fresh investigate (last investigate, bored 0-1, other near)")
    ap.add_argument("--seed", type=int, default=1104)
    ap.add_argument("--api", choices=("openai", "ollama", "rules"), default="openai")
    ap.add_argument("--host", default=None)
    ap.add_argument("--model", default=None)
    ap.add_argument("--prompt-file", default=None, help="candidate prompt, without editing gen_traces.py")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--retries", type=int, default=5)
    ap.add_argument("--max-requests", type=int, default=500,
                    help="refuse an unexpectedly large teacher run (top-ups must raise this explicitly)")
    ap.add_argument("--report-out", default=None, help="write prompt, rows, timing and metrics as JSON")
    ap.add_argument("--traces-out", default=None,
                    help="append/resume teacher-labelled JSONL for fold_traces.py")
    ap.add_argument("--dry-run", action="store_true", help="validate and summarize states; ask no teacher")
    args = ap.parse_args()

    for name, value in (("--a10", args.a10), ("--a12-pairs", args.a12_pairs), ("--a12x-pairs", args.a12x_pairs)):
        if value < 0:
            ap.error(f"{name} must be >= 0")
    if args.a10 + args.a12_pairs + args.a12x_pairs == 0:
        ap.error("the focused panel is empty")
    if args.workers < 1 or args.retries < 1:
        ap.error("--workers and --retries must be >= 1")
    if args.dry_run and args.traces_out:
        ap.error("--dry-run cannot write teacher traces")

    gt.SCHEMA = "L1"
    tok.set_schema("L1")
    if args.prompt_file:
        gt.SYSTEM_PROMPT_L1 = open(args.prompt_file).read().strip()
    prompt = gt.system_prompt()
    prompt_sha1 = hashlib.sha1(prompt.encode()).hexdigest()
    panel = focused_panel(random.Random(args.seed), args.a10, args.a12_pairs, args.a12x_pairs)
    if len(panel) > args.max_requests:
        ap.error(f"{len(panel)} calls > --max-requests {args.max_requests}")
    if args.dry_run:
        print(f"{len(panel)} valid focused states, seed {args.seed}; prompt sha1 {prompt_sha1[:10]}")
        print(f"  A10 {args.a10}; A12 {args.a12_pairs} matched pairs; A12x {args.a12x_pairs} matched pairs")
        for case, index, state in panel[:3]:
            print(f"  {case}[{index}] {state}")
        return

    host, model = gt.teacher_defaults(args.api, args.host, args.model)
    expected = {(case, index): state for case, index, state in panel}
    existing = (_read_trace_rows(args.traces_out, expected, args.seed, prompt_sha1)
                if args.traces_out else {})
    missing = [(case, index, state) for case, index, state in panel if (case, index) not in existing]
    print(f"{len(panel)} focused states; {len(existing)} already labelled, {len(missing)} calls to "
          f"{model or 'rules'} @ {host or 'local'} ({args.workers} workers); prompt sha1 {prompt_sha1[:10]}",
          flush=True)

    def ask(item):
        case, index, state = item
        errors = []
        for attempt in range(1, args.retries + 1):
            try:
                answer = gt.teacher_call(args.api, host, model, state, args.timeout)
                if answer["goal"] is not None:
                    return {
                        "sampler": SAMPLER,
                        "seed": args.seed,
                        "case": case,
                        "index": index,
                        "state": state,
                        "goal": answer["goal"],
                        "teacher_model": model,
                        "prompt_sha1": prompt_sha1,
                        "usage": answer["usage"],
                        "secs": answer["secs"],
                        "attempts": attempt,
                    }, None
                errors.append(answer["fail"] or "invalid reply")
            except Exception as exc:  # transport/server failures are retried per independent state
                errors.append(f"{type(exc).__name__}: {exc}"[:200])
            if attempt < args.retries:
                time.sleep(min(2.0, 0.25 * attempt))
        return None, "; ".join(errors[-2:])

    started = time.time()
    fresh, failures = {}, []
    trace_fh = None
    if args.traces_out:
        os.makedirs(os.path.dirname(args.traces_out) or ".", exist_ok=True)
        trace_fh = open(args.traces_out, "a", buffering=1)
    try:
        with cf.ThreadPoolExecutor(args.workers) as executor:
            for item, (row, error) in zip(missing, executor.map(ask, missing)):
                key = (item[0], item[1])
                if row is None:
                    failures.append({"case": item[0], "index": item[1], "error": error})
                    continue
                fresh[key] = row
                if trace_fh:
                    trace_fh.write(json.dumps(row) + "\n")
                done = len(existing) + len(fresh)
                if done % 100 == 0 or done == len(panel):
                    print(f"{done}/{len(panel)}", flush=True)
    finally:
        if trace_fh:
            trace_fh.close()

    all_rows = {**existing, **fresh}
    ordered = [all_rows[(case, index)] for case, index, _ in panel if (case, index) in all_rows]
    elapsed = time.time() - started
    metrics = summarize(panel, ordered)
    print_summary(metrics)
    print(f"elapsed {elapsed:.1f}s; failures {len(failures)}")

    if args.report_out:
        os.makedirs(os.path.dirname(args.report_out) or ".", exist_ok=True)
        report = {
            "sampler": SAMPLER,
            "seed": args.seed,
            "a10": args.a10,
            "a12_pairs": args.a12_pairs,
            "model": model,
            "host": host,
            "prompt_sha1": prompt_sha1,
            "prompt": prompt,
            "elapsed_secs": elapsed,
            "metrics": metrics,
            "failures": failures,
            "rows": ordered,
        }
        with open(args.report_out, "w") as fh:
            json.dump(report, fh, indent=1)
        print(f"-> {args.report_out}")
    if args.traces_out:
        print(f"-> {args.traces_out}")
    if failures or len(ordered) != len(panel):
        sys.exit(f"incomplete: {len(ordered)}/{len(panel)} teacher labels; rerun the same command to resume")


if __name__ == "__main__":
    main()

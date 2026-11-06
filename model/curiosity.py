#!/usr/bin/env python3
"""Matched L1 curiosity diagnostics and resumable, focused teacher labels.

  python3 curiosity.py panel --n 28 --seed 701 --out out/curiosity_panel.json
  .venv/bin/python curiosity.py student --ckpt out/a/ckpt.pt out/b/ckpt.pt --out out/curiosity_student.json
  python3 curiosity.py teacher --samples 3 --max-requests 100 --cache out/curiosity_cache.jsonl \
      --traces-out out/curiosity_labels.jsonl --report-out out/curiosity_teacher.json

Each background has curiosity 0/3/6/9 with ALL other fields held fixed. Calm
backgrounds are fed, comfortable, awake, calm and energetic; separate controls
exercise hunger, temperature, stress, exhaustion, sleep, shed and territorial
company. The numbers are descriptive, not new acceptance thresholds.

Only student mode imports the checkpoint/tokenizer code (and hence torch).
Teacher mode uses gen_traces' OpenAI client, defaults and LAB_API_KEY convention;
--prompt-file replaces SYSTEM_PROMPT_L1 only inside this process. Every returned
raw answer is durably cached, including invalid answers. Transport errors are
not cached, so a resume can retry them; invalid cached answers remain visible
errors, not automatic paid retries. A model alias remapped to different weights
needs a different --teacher-tag to avoid reusing that alias's previous answers.

--max-requests is a hard cap on NEW teacher_call attempts in this invocation,
not on cache hits. Sample 0 is requested across the panel before sample 1. An
error stops new submissions; already-running calls finish and enter the cache.
A capped, failed or interrupted run writes its partial report/valid labels and
exits nonzero. Rerun the same command to resume. Labels are rebuilt atomically
in stable panel/sample order as {state, goal}, with no stand-in or dataset mix.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
from pathlib import Path
import random
import sys
import tempfile
import threading
import time

import gen_traces as gt
import prompt_check as pc

SAMPLER = "l1-curiosity-v1"
CACHE_FORMAT = "l1-curiosity-teacher-v1"
DIGITS = (0, 3, 6, 9)
CASES = ("calm", "hungry", "hot", "cold", "stressed", "exhausted", "sleep", "shed", "low_social_near")
PRIORITY_GOALS = {
    "calm": (), "hungry": ("hunt",), "hot": ("hide",), "cold": ("bask",),
    "stressed": ("hide",), "exhausted": ("rest",), "sleep": ("rest", "hide"),
    "shed": ("hide",), "low_social_near": ("hide", "explore"),
}


def sha256(data):
    return hashlib.sha256(data.encode("utf-8") if isinstance(data, str) else data).hexdigest()


def json_key(value):
    return sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False))


def make_panel(n, seed):
    """A prefix-stable panel: increasing n does not change earlier backgrounds."""
    panel, seen = [], set()
    for background in range(n):
        for case in CASES:
            rng = random.Random(f"{SAMPLER}:{seed}:{case}:{background}")
            social = DIGITS[(background // 4) % len(DIGITS)]
            other = (f"{rng.choice(('near', 'mid', 'far'))} {rng.randint(1, 12)}"
                     if social >= 3 else "none")
            f = pc.fields_l1(
                rng, hunger=rng.randint(0, 2), energy=rng.randint(6, 9), stress=rng.randint(0, 2),
                heat=rng.randint(5, 6), bold=DIGITS[background % len(DIGITS)], social=social,
                stage=("hatch", "juv", "adult", "elder")[(background + background // 7) % 4],
                trust=DIGITS[(background + background // 4) % 4], bored=rng.randint(0, 2),
                last=gt.GOALS_L1[background % len(gt.GOALS_L1)], shed="no", phase="active", prey="none",
                other=other, warm=f"mid {rng.randint(1, 12)}", shelter=f"near {rng.randint(1, 12)}",
            )
            if case == "hungry":
                f.update(hunger=9, prey=f"near {rng.randint(1, 12)}")
            elif case == "hot":
                f["heat"] = 9
            elif case == "cold":
                f["heat"] = 1
            elif case == "stressed":
                f["stress"] = 9
            elif case == "exhausted":
                f["energy"] = 1
            elif case == "sleep":
                f["phase"] = "sleep"
            elif case == "shed":
                f["shed"] = "yes"
            elif case == "low_social_near":
                f.update(social=background % 3, other=f"near {rng.randint(1, 12)}")
            for curiosity in DIGITS:
                state = pc.line_l1(dict(f, curiosity=curiosity))
                if state in seen:
                    raise ValueError(f"duplicate panel state: {case}[{background}] curiosity {curiosity}")
                seen.add(state)
                panel.append({"case": case, "background": background, "curiosity": curiosity, "state": state})
    return panel


def panel_report(panel, n, seed):
    return {"sampler": SAMPLER, "n": n, "seed": seed, "state_count": len(panel),
            "panel_sha256": json_key(panel), "states": panel,
            "notes": [
                "Within each (case, background), only curiosity differs across 0/3/6/9.",
                "Probabilities are means across answered states; teacher distributions use valid answers only.",
                "High-low differences use matched backgrounds with both endpoint distributions available.",
                "No new acceptance thresholds; low-social hide/explore mass is descriptive, not an exhaustive allowed set.",
            ]}


def atomic_text(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    name = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", delete=False) as fh:
            name = fh.name
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(name, path)
    finally:
        if name is not None and os.path.exists(name):
            os.unlink(name)


def write_json(path, value):
    atomic_text(path, json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n")


def mean(values):
    return sum(values) / len(values) if values else None


def mass(dist, goals):
    return sum(dist.get(goal, 0.0) for goal in goals)


def summarize(panel, distributions):
    """Equal weight per answered state, including when teacher sample counts differ."""
    cases = {}
    for case in CASES:
        indices = [i for i, row in enumerate(panel) if row["case"] == case]
        priority = PRIORITY_GOALS[case]
        by_digit = {}
        for digit in DIGITS:
            selected = [i for i in indices if panel[i]["curiosity"] == digit]
            ds = [distributions[i] for i in selected if distributions[i] is not None]
            by_digit[str(digit)] = {
                "states": len(selected), "answered_states": len(ds),
                "explore": mean([d["explore"] for d in ds]),
                "explore_investigate": mean([mass(d, ("explore", "investigate")) for d in ds]),
                "rest_hide": mean([mass(d, ("rest", "hide")) for d in ds]),
                "greedy_rest_hide_fraction": mean([float(max(d, key=d.get) in ("rest", "hide")) for d in ds]),
                "priority": mean([mass(d, priority) for d in ds]) if priority else None,
            }
        endpoints = {}
        for i in indices:
            row = panel[i]
            if row["curiosity"] in (0, 9):
                endpoints.setdefault(row["background"], {})[row["curiosity"]] = i
        changes = []
        for background, pair in endpoints.items():
            lo, hi = pair[0], pair[9]
            low, high = distributions[lo], distributions[hi]
            if low is None or high is None:
                continue
            changes.append({
                "background": background, "low_state_index": lo, "high_state_index": hi,
                "explore": high["explore"] - low["explore"],
                "explore_investigate": mass(high, ("explore", "investigate")) - mass(low, ("explore", "investigate")),
                "rest_hide": mass(high, ("rest", "hide")) - mass(low, ("rest", "hide")),
                "priority": mass(high, priority) - mass(low, priority) if priority else None,
            })
        paired = {name: mean([change[name] for change in changes])
                  for name in ("explore", "explore_investigate", "rest_hide")}
        paired.update(pairs=len(changes), changes=changes,
                      priority=mean([change["priority"] for change in changes]) if priority else None)
        ds = [distributions[i] for i in indices if distributions[i] is not None]
        cases[case] = {"priority_goals": list(priority), "by_curiosity": by_digit,
                       "paired_high_low": paired,
                       "priority_probability": mean([mass(d, priority) for d in ds]) if priority else None}
    territorial = []
    for row, dist in zip(panel, distributions):
        f = gt.parse_l1_line(row["state"])
        if f["social"] <= 2 and f["other"] is not None and f["other"][0] == "near" and dist is not None:
            territorial.append(dist["investigate"])
    calm_low = cases["calm"]["by_curiosity"]["0"]
    return {"cases": cases,
            "low_curiosity_rest_hide": {"case": "calm", "curiosity": 0,
                                         "answered_states": calm_low["answered_states"],
                                         "probability": calm_low["rest_hide"],
                                         "greedy_fraction": calm_low["greedy_rest_hide_fraction"]},
            "inappropriate_investigate": {"condition": "social <= 2 and other near",
                                           "answered_states": len(territorial), "probability": mean(territorial)}}


def number(value, signed=False):
    return "n/a" if value is None else (f"{value:+.4f}" if signed else f"{value:.4f}")


def print_metrics(metrics):
    print("  curiosity digits:       " + " ".join(f"{digit:>8}" for digit in DIGITS))
    for case, values in metrics["cases"].items():
        for key, label in (("explore", "P(explore)"), ("explore_investigate", "P(explore+investigate)")):
            print(f"  {case:16s} {label:23s} " + " ".join(
                f"{number(values['by_curiosity'][str(digit)][key]):>8}" for digit in DIGITS))
        pair = values["paired_high_low"]
        print(f"    paired 9-0 ({pair['pairs']} backgrounds): explore {number(pair['explore'], True)}, "
              f"explore+investigate {number(pair['explore_investigate'], True)}")
        if values["priority_goals"]:
            print(f"    priority {'+'.join(values['priority_goals'])}: " + " ".join(
                number(values["by_curiosity"][str(digit)]["priority"]) for digit in DIGITS))
    low, bad = metrics["low_curiosity_rest_hide"], metrics["inappropriate_investigate"]
    print(f"  calm curiosity 0 rest/hide: P={number(low['probability'])}, "
          f"greedy fraction={number(low['greedy_fraction'])} (n={low['answered_states']})")
    print(f"  inappropriate investigate (low-social other near): {number(bad['probability'])} "
          f"(n={bad['answered_states']}); descriptive only, no thresholds")


def cache_key(identity, state, sample):
    return json_key([identity, state, sample])


class TeacherCache:
    """Single-writer, append-only raw answers; only a torn final record is repaired."""

    def __init__(self, path):
        import fcntl
        self.path, self.rows, self.lock = Path(path), {}, threading.Lock()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fh = self.path.open("a+b")
        try:
            try:
                fcntl.flock(self.fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError(f"{path}: another process is using this teacher cache") from None
            self.fh.seek(0)
            size = os.fstat(self.fh.fileno()).st_size
            while True:
                start = self.fh.tell()
                raw = self.fh.readline()
                if not raw:
                    break
                try:
                    record = json.loads(raw)
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    if self.fh.tell() == size and not raw.endswith(b"\n"):
                        self.fh.truncate(start)
                        self.fh.flush()
                        os.fsync(self.fh.fileno())
                        print(f"recovered torn final cache record in {path}", file=sys.stderr)
                        break
                    raise ValueError(f"{path}: malformed cache record at byte {start}: {exc}") from None
                self.validate(record)
                key = record["key"]
                if key in self.rows and self.rows[key] != record:
                    raise ValueError(f"{path}: conflicting cached answers for {key}")
                self.rows[key] = record
                if not raw.endswith(b"\n"):
                    self.fh.write(b"\n")
                    self.fh.flush()
                    os.fsync(self.fh.fileno())
            self.fh.seek(0, os.SEEK_END)
        except BaseException:
            self.fh.close()
            raise

    def validate(self, record):
        if not isinstance(record, dict) or record.get("format") != CACHE_FORMAT:
            raise ValueError(f"{self.path}: not a {CACHE_FORMAT} cache record")
        identity, state, sample = record.get("identity"), record.get("state"), record.get("sample")
        if (not isinstance(identity, dict) or not isinstance(state, str) or gt.check_l1_line(state)
                or type(sample) is not int or sample < 0 or not isinstance(record.get("raw"), str)):
            raise ValueError(f"{self.path}: invalid cached identity/state/sample/raw answer")
        required = ("api", "host", "model", "prompt_sha256", "teacher_code_sha256", "tag")
        if any(not isinstance(identity.get(key), str) for key in required):
            raise ValueError(f"{self.path}: missing teacher identity fields")
        if record.get("key") != cache_key(identity, state, sample):
            raise ValueError(f"{self.path}: cache key does not match its identity/state/sample")
        goal, fail = gt.parse_decision(record["raw"])
        if record.get("goal") != goal or record.get("fail") != fail:
            raise ValueError(f"{self.path}: cached parsed goal does not match its raw answer")

    def put(self, record):
        data = (json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        with self.lock:
            if record["key"] in self.rows:
                raise ValueError(f"{self.path}: duplicate request for {record['key']}")
            self.fh.write(data)
            self.fh.flush()
            os.fsync(self.fh.fileno())
            self.rows[record["key"]] = record

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.fh.close()


def collect(panel, samples, identity, cache, workers, cap, timeout):
    jobs, hits, invalid_cached = [], 0, False
    for sample in range(samples):
        for index, row in enumerate(panel):
            key = cache_key(identity, row["state"], sample)
            record = cache.rows.get(key)
            if record is None:
                jobs.append((index, sample, key))
            else:
                hits += 1
                invalid_cached |= record["goal"] is None
    stats = {"requests": 0, "cache_hits": hits, "not_asked_cap": max(0, len(jobs) - cap),
             "aborted": invalid_cached, "interrupted": False}
    errors, lock = [], threading.Lock()

    def ask(job):
        index, sample, key = job
        state = panel[index]["state"]
        with lock:
            stats["requests"] += 1
        try:
            answer = gt.teacher_call(identity["api"], identity["host"], identity["model"], state, timeout)
        except Exception as exc:
            return {"kind": "transport_error", "state_index": index, "sample": sample,
                    "error": f"{type(exc).__name__}: {exc}"}
        record = dict(answer, format=CACHE_FORMAT, key=key, identity=identity, state=state, sample=sample)
        cache.put(record)
        return ({"kind": "invalid_answer", "state_index": index, "sample": sample, "error": answer["fail"]}
                if answer["goal"] is None else None)

    if not invalid_cached and cap and jobs:
        selected = iter(jobs[:cap])
        executor = cf.ThreadPoolExecutor(max_workers=workers)
        pending = set()
        handled = set()

        def handle(future):
            handled.add(future)
            if future.cancelled():
                return
            error = future.result()
            if error:
                stats["aborted"] = True
                if error["kind"] == "transport_error":
                    errors.append(error)
                    print(f"teacher error: {error['error']}", file=sys.stderr, flush=True)

        try:
            for _ in range(min(workers, len(jobs), cap)):
                pending.add(executor.submit(ask, next(selected)))
            while pending:
                done, pending = cf.wait(pending, return_when=cf.FIRST_COMPLETED)
                for future in done:
                    handle(future)
                if not stats["aborted"]:
                    for _ in done:
                        job = next(selected, None)
                        if job is not None:
                            pending.add(executor.submit(ask, job))
        except KeyboardInterrupt:
            stats["interrupted"] = True
            stats["aborted"] = True
            print("interrupted: preserving already-running teacher answers before writing partial outputs", file=sys.stderr)
        finally:
            for future in pending:
                future.cancel()
            executor.shutdown(wait=True, cancel_futures=True)
            for future in pending - handled:
                handle(future)
    answers = [[cache.rows.get(cache_key(identity, row["state"], sample)) for sample in range(samples)]
               for row in panel]
    invalid = [{"kind": "invalid_answer", "state_index": i, "sample": sample, "error": record["fail"]}
               for i, row in enumerate(answers) for sample, record in enumerate(row)
               if record is not None and record["goal"] is None]
    errors.extend(invalid)
    if stats["interrupted"]:
        errors.append({"kind": "interrupted", "error": "interrupted by user"})
    valid = sum(record is not None and record["goal"] is not None for row in answers for record in row)
    stats.update(valid_answers=valid, invalid_answers=len(invalid),
                 transport_errors=sum(error["kind"] == "transport_error" for error in errors),
                 missing=len(panel) * samples - valid - len(invalid))
    return answers, stats, errors


def run_student(args, panel):
    from probe_l1 import Student
    import train_tokenizer as tok
    tok.set_schema("L1")
    checkpoints = [path for group in args.ckpt for path in group]
    require_distinct([args.out], checkpoints)
    report = dict(panel_report(panel, args.n, args.seed), mode="student", temperature=args.temp,
                  device=args.device, checkpoints=[])
    for path in checkpoints:
        student = Student(path, args.temp, args.device)
        results = student.dists([row["state"] for row in panel])
        distributions = [dist for dist, _ in results]
        metrics = summarize(panel, distributions)
        report["checkpoints"].append({"ckpt": str(Path(path).resolve()), "checkpoint": student.info,
                                       "distributions": distributions, "goal_masses": [value for _, value in results],
                                       "metrics": metrics})
        print(f"{path}: {len(panel)} matched states, temperature {args.temp}")
        print_metrics(metrics)
        del student
    write_json(args.out, report)
    print(f"-> {args.out}")
    return 0


def run_teacher(args, panel):
    require_distinct([args.cache, args.traces_out, args.report_out], [args.prompt_file] if args.prompt_file else [])
    prompt = Path(args.prompt_file).read_text(encoding="utf-8").strip() if args.prompt_file else gt.SYSTEM_PROMPT_L1
    if not prompt:
        raise ValueError("--prompt-file is empty")
    host, model = gt.teacher_defaults(args.api, args.host, args.model)
    identity = {"api": args.api, "host": host.rstrip("/"), "model": model, "prompt_sha256": sha256(prompt),
                "teacher_code_sha256": sha256(Path(gt.__file__).read_bytes()), "tag": args.teacher_tag}
    report = dict(panel_report(panel, args.n, args.seed), mode="teacher", samples=args.samples,
                  expected_labels=len(panel) * args.samples, prompt_sha256=identity["prompt_sha256"],
                  prompt=prompt, teacher=identity, cache_path=str(Path(args.cache).resolve()),
                  traces_path=str(Path(args.traces_out).resolve()))
    started, original_prompt = time.monotonic(), gt.SYSTEM_PROMPT_L1
    gt.SYSTEM_PROMPT_L1 = prompt
    try:
        with TeacherCache(args.cache) as cache:
            answers, stats, errors = collect(panel, args.samples, identity, cache, args.workers,
                                            args.max_requests, args.timeout)
    finally:
        gt.SYSTEM_PROMPT_L1 = original_prompt
    distributions, valid_counts, lines = [], [], []
    for row, records in zip(panel, answers):
        valid = [record for record in records if record is not None and record["goal"] is not None]
        valid_counts.append(len(valid))
        counts = {goal: 0 for goal in gt.GOALS_L1}
        for record in valid:
            counts[record["goal"].split()[0]] += 1
            lines.append(json.dumps({"state": row["state"], "goal": record["goal"]}, ensure_ascii=False) + "\n")
        distributions.append({goal: count / len(valid) for goal, count in counts.items()} if valid else None)
    trace_text = "".join(lines)
    atomic_text(args.traces_out, trace_text)
    complete = stats["valid_answers"] == report["expected_labels"] and not errors
    metrics = summarize(panel, distributions)
    report.update(complete=complete, trace_count=len(lines), traces_sha256=sha256(trace_text), stats=stats,
                  errors=errors, elapsed_secs=time.monotonic() - started, distributions=distributions,
                  valid_samples_per_state=valid_counts, metrics=metrics)
    write_json(args.report_out, report)
    print(f"teacher {model}: {len(lines)}/{report['expected_labels']} valid labels; "
          f"{stats['requests']} new requests, {stats['cache_hits']} cache hits")
    print_metrics(metrics)
    print(f"-> {args.traces_out}\n-> {args.report_out}")
    for error in errors:
        if error["kind"] != "transport_error":
            print(f"teacher error: {error}", file=sys.stderr)
    if not complete:
        print(f"incomplete: {stats['missing']} missing, {stats['invalid_answers']} invalid; "
              "valid answers preserved. Resume uses the cache; invalid cached answers are not retried.", file=sys.stderr)
    return 0 if complete else 1


def require_distinct(outputs, inputs=()):
    resolved = [Path(path).resolve() for path in outputs]
    if len(set(resolved)) != len(resolved) or set(resolved) & {Path(path).resolve() for path in inputs}:
        raise ValueError("output paths must be distinct from each other and from input files")


def positive(value):
    value = int(value)
    if value < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return value


def nonnegative(value):
    value = int(value)
    if value < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return value


def positive_float(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be finite and > 0")
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = ap.add_subparsers(dest="mode", required=True)
    parsers = {}
    for name, help_text in (("panel", "print/export the deterministic panel, offline"),
                            ("student", "compare checkpoint probabilities on the same panel"),
                            ("teacher", "cache OpenAI teacher answers and emit focused labels")):
        parser = modes.add_parser(name, help=help_text)
        parser.add_argument("--n", type=positive, default=28, help="matched backgrounds per case (states = n * 9 * 4)")
        parser.add_argument("--seed", type=int, default=701)
        parsers[name] = parser
    parsers["panel"].add_argument("--out", help="panel JSON path; absent: print the complete JSON to stdout")
    student = parsers["student"]
    student.add_argument("--ckpt", action="append", nargs="+", required=True, help="checkpoint paths; lists/repetition supported")
    student.add_argument("--out", required=True, help="comparison JSON with states and aligned distributions")
    student.add_argument("--temp", type=positive_float, default=1.0, help="goal-softmax temperature (device: 1.0)")
    student.add_argument("--device", default="cpu", help="checkpoint inference device, e.g. cpu or cuda")
    teacher = parsers["teacher"]
    teacher.add_argument("--api", choices=("openai",), default="openai", help="gen_traces OpenAI client; key from LAB_API_KEY")
    teacher.add_argument("--host", help="base URL; gen_traces.teacher_defaults supplies the default")
    teacher.add_argument("--model", help="model name; gen_traces.teacher_defaults supplies the default")
    teacher.add_argument("--teacher-tag", default="", help="optional model revision/cache identity, e.g. after an alias remap")
    teacher.add_argument("--prompt-file", help="candidate SYSTEM_PROMPT_L1 as UTF-8 text; stripped at both ends")
    teacher.add_argument("--workers", type=positive, default=4)
    teacher.add_argument("--samples", type=positive, default=3, help="independent answers per state")
    teacher.add_argument("--timeout", type=positive_float, default=30.0, help="seconds per teacher call")
    teacher.add_argument("--max-requests", type=nonnegative, required=True, help="hard NEW-call cap; 0 exports cache only")
    teacher.add_argument("--cache", required=True, help="append-only raw teacher response JSONL; reuse on resume")
    teacher.add_argument("--traces-out", required=True, help="atomic focused {state,goal} JSONL (valid answers only)")
    teacher.add_argument("--report-out", required=True, help="JSON provenance, completion, errors and probability diagnostics")
    args = ap.parse_args()
    gt.SCHEMA = "L1"
    try:
        panel = make_panel(args.n, args.seed)
        if args.mode == "panel":
            report = dict(panel_report(panel, args.n, args.seed), mode="panel")
            if args.out:
                write_json(args.out, report)
                print(f"{len(panel)} matched states ({args.n} backgrounds x {len(CASES)} cases x {len(DIGITS)} digits) -> {args.out}")
            else:
                print(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False))
            return 0
        return run_student(args, panel) if args.mode == "student" else run_teacher(args, panel)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

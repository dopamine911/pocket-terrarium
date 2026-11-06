#!/usr/bin/env python3
"""The aquarium sim's census for any fish model/tokenizer pair, without editing sim code.

  python3 fish_census.py [--minutes N] [--repeats K] [--jobs J] [--json out.json] [--markdown out.md]
                         NAME=MODEL_Q4[,TOKENIZER] | NAME=RUN_DIR ...

A candidate is NAME=path/to/model_q4.bin[,path/to/tokenizer.bin], or NAME=<a directory holding
model_q4.bin> (a harness job directory, out/harness/<run>/s<seed>/; its tokenizer.bin if it has
one). The tokenizer defaults to the repository's model/out/tokenizer.bin (the fish schema-4 one).

The sim's `--selftest-llm [minutes]` reads ../model/out/model_q4.bin and ../model/out/tokenizer.bin
relative to its working directory (PT_MODEL_FILE / PT_TOKENIZER_FILE for the aquarium, common/world.h).
For every candidate (and repeat) this script makes a scratch directory <tmp>/<name>/{sim,model/out}:
model/out/model_q4.bin and tokenizer.bin are symlinks to the candidate's files, sim/ holds symlinks to
the repository's sim/ entries, and the other top-level repository entries are symlinked beside it.
The binary runs from <tmp>/<name>/sim, so no sim code or model/out is touched and several candidates
run side by side (--jobs).

--minutes 0 is the fast smoke (one simulated hour, ~2 ms a tick: about a minute, mostly inference);
--minutes N > 0 is real-time pacing for N minutes per run (the honest measure of survival
overrides, which depend on how long a decision takes). Real-time runs compete for cores: keep --jobs
at or below the number of physical cores, or the slower decisions inflate the overrides.

The sim's seed is fixed (tank_init 4321), so repeats differ by the model's sampling only;
--repeats K reports the mean and range of each number over K runs.

Output per run, parsed from sim/main.c selftest_llm:
  selftest-llm: C goal changes (T torn), A asks, O survival overrides in S sim-seconds
  census: goal share <goal> P% ...
  census: fish-time at a landmark ... L% | 3+ fish crowding one R% of the time | a 3-fish cluster anywhere K% |
          mean curiosity c hunger h energy e
  census: explore - Z distinct zones per fish-minute | longest one-goal stretch G s | mean bored B

Build the binary first (not done here), from the repository root, with the terrarium's SDL setup:
  S=~/.cache/pocket-terrarium/sdl2/root
  make -C sim -j12 WORLD=aquarium EXTRA_CFLAGS=-D_DEFAULT_SOURCE \\
       SDL_CFLAGS="-I$S/usr/include -I$S/usr/include/SDL2 -I$S/usr/include/x86_64-linux-gnu" \\
       SDL_LIBS="-l:libSDL2-2.0.so.0"
which makes sim/fishsim-aquarium (sim/lvgl may need copying from ../pt-dry/sim/lvgl). Add
LD_LIBRARY_PATH for the SDL2 root if the loader cannot find libSDL2-2.0.so.0.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_BINARY = os.path.join(REPO, "sim", "fishsim-aquarium")
DEFAULT_TOKENIZER = os.path.join(HERE, "out", "tokenizer.bin")

SELFTEST_RE = re.compile(r"selftest-llm: (\d+) goal changes \((\d+) torn\), (\d+) asks, (\d+) survival overrides "
                         r"in (\d+) sim-seconds")
SHARE_RE = re.compile(r"census: goal share(.*)")
SHARE_ITEM_RE = re.compile(r"(\w+) (\d+)%")
LANDMARK_RE = re.compile(r"census: fish-time at a landmark \([^)]*\) (\d+)% \| 3\+ fish crowding one (\d+)% of the time \| "
                         r"a 3-fish cluster anywhere (\d+)% \| mean curiosity (-?[\d.]+) hunger (-?[\d.]+) energy (-?[\d.]+)")
EXPLORE_RE = re.compile(r"census: explore - (-?[\d.]+) distinct zones per fish-minute \| longest one-goal stretch (\d+) s \| "
                        r"mean bored (-?[\d.]+)")

# metric key -> (column header, format); goal shares are added per goal
METRICS = [
    ("changes", "goal changes", "{:.0f}"), ("torn", "torn", "{:.0f}"), ("asks", "asks", "{:.0f}"),
    ("overrides", "overrides", "{:.0f}"), ("sim_seconds", "sim s", "{:.0f}"),
    ("landmark", "at landmark %", "{:.0f}"), ("crowd", "3+ crowd %", "{:.0f}"), ("cluster", "3-cluster %", "{:.0f}"),
    ("curiosity", "curiosity", "{:.1f}"), ("hunger", "hunger", "{:.1f}"), ("energy", "energy", "{:.1f}"),
    ("zones", "zones/fish-min", "{:.1f}"), ("longest", "longest stretch s", "{:.0f}"), ("bored", "bored", "{:.1f}"),
]


class CensusError(Exception):
    pass


def parse_output(text):
    """the census numbers of one selftest-llm run; CensusError if a census line is missing"""
    out = {"goal_share": {}}
    m = None
    for line in text.splitlines():
        line = line.strip()
        if (m := SELFTEST_RE.search(line)):
            out.update(changes=int(m[1]), torn=int(m[2]), asks=int(m[3]), overrides=int(m[4]), sim_seconds=int(m[5]))
        elif (m := SHARE_RE.match(line)):
            out["goal_share"] = {g: int(p) for g, p in SHARE_ITEM_RE.findall(m[1])}
        elif (m := LANDMARK_RE.match(line)):
            out.update(landmark=int(m[1]), crowd=int(m[2]), cluster=int(m[3]),
                       curiosity=float(m[4]), hunger=float(m[5]), energy=float(m[6]))
        elif (m := EXPLORE_RE.match(line)):
            out.update(zones=float(m[1]), longest=int(m[2]), bored=float(m[3]))
    need = [k for k, _, _ in METRICS if k not in out]
    if need or not out["goal_share"]:
        missing = need + ([] if out["goal_share"] else ["goal_share"])
        why = "the model/tokenizer were not found" if "FAIL:" in text else "census lines missing"
        raise CensusError(f"{why}: {', '.join(missing)}")
    return out


def summarize(runs):
    """mean, min, max of every number over the successful repeats of one candidate"""
    def agg(vals):
        return {"mean": statistics.fmean(vals), "min": min(vals), "max": max(vals)}
    ok = [r for r in runs if "result" in r]
    if not ok:
        return None
    res = [r["result"] for r in ok]
    s = {k: agg([r[k] for r in res]) for k, _, _ in METRICS}
    goals = sorted({g for r in res for g in r["goal_share"]})
    s["goal_share"] = {g: agg([r["goal_share"].get(g, 0) for r in res]) for g in goals}
    return s


def cell(a, fmt, repeats):
    if a is None:
        return "-"
    if repeats <= 1 or a["min"] == a["max"]:
        return fmt.format(a["mean"])
    return f"{fmt.format(a['mean'])} ({fmt.format(a['min'])}-{fmt.format(a['max'])})"


def markdown(report):
    cands, k = report["candidates"], report["repeats"]
    names = list(cands)
    lines = [f"Aquarium `--selftest-llm` census: {report['minutes']} min per run "
             f"({'fast smoke, one simulated hour' if report['minutes'] == 0 else 'real time'}), "
             f"{k} repeat{'s' if k != 1 else ''}" + (" (mean, range)." if k > 1 else "."), ""]
    head = ["candidate", "ok"] + [h for _, h, _ in METRICS]
    lines += ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for n in names:
        c = cands[n]
        s = c["summary"]
        row = [n, f"{c['ok']}/{len(c['runs'])}"]
        row += [cell(s[key] if s else None, fmt, k) for key, _, fmt in METRICS]
        lines.append("| " + " | ".join(row) + " |")
    goals = sorted({g for n in names if cands[n]["summary"] for g in cands[n]["summary"]["goal_share"]})
    if goals:
        lines += ["", "Goal share (% of fish-time):", "",
                  "| candidate | " + " | ".join(goals) + " |", "|" + "---|" * (len(goals) + 1)]
        for n in names:
            s = cands[n]["summary"]
            row = [cell(s["goal_share"].get(g) if s else None, "{:.0f}", k) for g in goals]
            lines.append(f"| {n} | " + " | ".join(row) + " |")
    errs = [(n, i, r["error"]) for n in names for i, r in enumerate(cands[n]["runs"]) if "error" in r]
    if errs:
        lines += ["", "Failed runs:", ""] + [f"- {n} repeat {i + 1}: {e}" for n, i, e in errs]
    return "\n".join(lines) + "\n"


def resolve(spec):
    """NAME=MODEL[,TOKENIZER] or NAME=DIR -> (name, model path, tokenizer path)"""
    name, sep, rest = spec.partition("=")
    if not sep or not name or not rest or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise CensusError(f"candidate {spec!r}: expected NAME=MODEL_Q4[,TOKENIZER] or NAME=DIR (name: letters, digits, . _ -)")
    model, _, tok = rest.partition(",")
    model = os.path.abspath(model)
    if os.path.isdir(model):
        d = model
        model = os.path.join(d, "model_q4.bin")
        if not tok and os.path.exists(os.path.join(d, "tokenizer.bin")):
            tok = os.path.join(d, "tokenizer.bin")
    tok = os.path.abspath(tok) if tok else DEFAULT_TOKENIZER
    for what, p in (("model", model), ("tokenizer", tok)):
        if not os.path.isfile(p):
            raise CensusError(f"candidate {name}: {what} {p} not found")
    return name, model, tok


def make_scratch(base, name, model, tok, repo=REPO):
    """<base>/<name>/{sim,model/out}; returns the sim directory to run from"""
    root = os.path.join(base, name)
    sim = os.path.join(root, "sim")
    out = os.path.join(root, "model", "out")
    os.makedirs(sim)
    os.makedirs(out)
    for entry in os.listdir(repo):                   # assets/, common/ ... beside sim/ and model/, in case a path needs them
        if entry not in ("sim", "model", ".git"):
            os.symlink(os.path.join(repo, entry), os.path.join(root, entry))
    for entry in os.listdir(os.path.join(repo, "sim")):
        os.symlink(os.path.join(repo, "sim", entry), os.path.join(sim, entry))
    os.symlink(model, os.path.join(out, "model_q4.bin"))
    os.symlink(tok, os.path.join(out, "tokenizer.bin"))
    return sim


def run_one(binary, base, name, model, tok, minutes, timeout, keep):
    scratch = os.path.join(base, name)
    try:
        sim = make_scratch(base, name, model, tok)
        try:
            p = subprocess.run([binary, "--selftest-llm", str(minutes)], cwd=sim, capture_output=True, text=True,
                               timeout=timeout, env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"))
        except subprocess.TimeoutExpired:
            return {"error": f"timed out after {timeout} s"}
        except OSError as e:
            return {"error": f"cannot run {binary}: {e}"}
        try:
            return {"result": parse_output(p.stdout), "rc": p.returncode}
        except CensusError as e:
            return {"error": f"{e} (rc {p.returncode}; {(p.stderr or p.stdout).strip()[-200:]!r})"}
    finally:
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)


def census(cands, binary, minutes, repeats, jobs, tmp, keep):
    timeout = int(minutes * 60 * 1.5 + 600)
    base = tempfile.mkdtemp(prefix="fish-census-", dir=tmp)
    report = {"binary": binary, "minutes": minutes, "repeats": repeats, "candidates": {}}
    try:
        with cf.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
            futs = {}
            for name, model, tok in cands:
                report["candidates"][name] = {"model": model, "tokenizer": tok, "runs": [None] * repeats}
                for r in range(repeats):
                    futs[ex.submit(run_one, binary, base, f"{name}-r{r + 1}", model, tok, minutes, timeout, keep)] = (name, r)
            for f in cf.as_completed(futs):
                name, r = futs[f]
                report["candidates"][name]["runs"][r] = f.result()
                print(f"{name} repeat {r + 1}/{repeats}: {'ok' if 'result' in f.result() else f.result()['error']}",
                      file=sys.stderr)
    finally:
        if keep:
            print(f"scratch directories kept under {base}", file=sys.stderr)
        else:
            shutil.rmtree(base, ignore_errors=True)
    for c in report["candidates"].values():
        c["ok"] = sum("result" in r for r in c["runs"])
        c["summary"] = summarize(c["runs"])
    return report


CANNED = """warm-up (pages in the mmap'd weights):
  t=3.2s Bubba -> follow (urgency 4, p=0.71)
selftest-llm: 57 goal changes (9 torn), 63 asks, 2 survival overrides in 3600 sim-seconds
census: goal share explore 31% seek_food 12% rest 22% follow 18% bubbles 9% flee 0% dart 8%
census: fish-time at a landmark (bubbles/reef, 55 px) 34% | 3+ fish crowding one 12% of the time | a 3-fish cluster anywhere 7% | mean curiosity 5.4 hunger 3.2 energy 6.1
census: explore - 2.8 distinct zones per fish-minute | longest one-goal stretch 75 s | mean bored 2.3
"""


def selftest():
    r = parse_output(CANNED)
    assert (r["changes"], r["torn"], r["asks"], r["overrides"], r["sim_seconds"]) == (57, 9, 63, 2, 3600), r
    assert r["goal_share"] == {"explore": 31, "seek_food": 12, "rest": 22, "follow": 18, "bubbles": 9, "flee": 0, "dart": 8}, r
    assert (r["landmark"], r["crowd"], r["cluster"]) == (34, 12, 7) and (r["curiosity"], r["hunger"], r["energy"]) == (5.4, 3.2, 6.1), r
    assert (r["zones"], r["longest"], r["bored"]) == (2.8, 75, 2.3), r
    for bad in ("FAIL: model.bin/tokenizer.bin not found under ../model/out/\n", CANNED.replace("census: explore", "x")):
        try:
            parse_output(bad)
        except CensusError:
            pass
        else:
            raise AssertionError("a run without its census lines parsed")

    tmp = tempfile.mkdtemp(prefix="fish-census-selftest-")
    try:
        # a fake repository, a fake candidate and a fake binary that checks the layout it runs in
        repo = os.path.join(tmp, "repo")
        os.makedirs(os.path.join(repo, "sim"))
        os.makedirs(os.path.join(repo, "assets"))
        open(os.path.join(repo, "sim", "data.txt"), "w").write("x")
        model, tok = os.path.join(tmp, "m.bin"), os.path.join(tmp, "t.bin")
        open(model, "w").write("MODEL")
        open(tok, "w").write("TOK")
        sim = make_scratch(os.path.join(tmp, "scratch"), "cand-r1", model, tok, repo=repo)
        assert open(os.path.join(sim, "..", "model", "out", "model_q4.bin")).read() == "MODEL"
        assert open(os.path.join(sim, "..", "model", "out", "tokenizer.bin")).read() == "TOK"
        assert open(os.path.join(sim, "data.txt")).read() == "x" and os.path.isdir(os.path.join(sim, "..", "assets"))
        fake = os.path.join(tmp, "fake-sim")
        with open(fake, "w") as fh:
            fh.write("#!/bin/sh\nif [ \"$1\" != --selftest-llm ] || [ ! -r ../model/out/model_q4.bin ] || [ ! -r ../model/out/tokenizer.bin ]; then\n"
                     "  echo 'FAIL: model.bin/tokenizer.bin not found under ../model/out/'; exit 1; fi\n"
                     f"cat <<'EOF'\n{CANNED}EOF\n")
        os.chmod(fake, 0o755)
        rep = census([("cand", model, tok), ("cand2", model, tok)], fake, 0, 2, 2, tmp, False)
        assert [c["ok"] for c in rep["candidates"].values()] == [2, 2], rep
        s = rep["candidates"]["cand"]["summary"]
        assert s["changes"] == {"mean": 57, "min": 57, "max": 57} and s["goal_share"]["dart"]["mean"] == 8, s
        assert not [e for e in os.listdir(tmp) if e.startswith("fish-census-")], "scratch directories left behind"
        # repeats that differ: mean and range
        runs = [{"result": dict(r, changes=50)}, {"result": dict(r, changes=60)}, {"error": "x"}]
        s = summarize(runs)
        assert s["changes"] == {"mean": 55, "min": 50, "max": 60}, s
        rep["candidates"]["cand"].update(runs=runs, ok=2, summary=s)
        md = markdown(rep)
        assert "55 (50-60)" in md and "| cand2 |" in md and "Failed runs" in md and "| candidate | dart |" not in md, md
        # a binary that finds nothing is reported, not raised
        bad = census([("nocand", model, tok)], os.path.join(tmp, "missing-binary"), 0, 1, 1, tmp, False)
        assert bad["candidates"]["nocand"]["ok"] == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("fish_census selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("candidates", nargs="*", help="NAME=MODEL_Q4[,TOKENIZER] or NAME=DIR")
    ap.add_argument("--binary", default=DEFAULT_BINARY, help="the aquarium simulator (default sim/fishsim-aquarium)")
    ap.add_argument("--minutes", type=int, default=0, help="0: fast smoke; N > 0: real-time minutes per run")
    ap.add_argument("--repeats", type=int, default=1, help="runs per candidate (the sim's seed is fixed; sampling differs)")
    ap.add_argument("--jobs", type=int, default=1, help="runs in parallel")
    ap.add_argument("--tmp", help="parent of the scratch directories (default: the system temp dir)")
    ap.add_argument("--keep", action="store_true", help="keep the scratch directories")
    ap.add_argument("--json", help="write the full results here")
    ap.add_argument("--markdown", help="write the table here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.candidates:
        ap.error("give at least one NAME=MODEL_Q4[,TOKENIZER] candidate")
    if args.repeats < 1 or args.minutes < 0 or args.jobs < 1:
        ap.error("--repeats and --jobs must be at least 1, --minutes at least 0")
    try:
        cands = [resolve(s) for s in args.candidates]
        if len({c[0] for c in cands}) != len(cands):
            raise CensusError("candidate names must be unique")
    except CensusError as e:
        sys.exit(str(e))
    binary = os.path.abspath(args.binary)
    if not os.access(binary, os.X_OK):
        sys.exit(f"{binary} is not built; see the build command in this script's docstring")
    report = census(cands, binary, args.minutes, args.repeats, args.jobs, args.tmp, args.keep)
    md = markdown(report)
    print(md)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(report, fh, indent=2)
    if args.markdown:
        with open(args.markdown, "w") as fh:
            fh.write(md)
    return 0 if all(c["ok"] == args.repeats for c in report["candidates"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())

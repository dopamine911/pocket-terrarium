#!/usr/bin/env python3
"""Teacher agreement of L1 students on a fixed state set: a measure that can separate models.

  cd model
  POCKET_SCHEMA=L1 .venv/bin/python agree.py --ckpt out/a/ckpt.pt --ckpt out/b/ckpt.pt   # the lab teacher
  POCKET_SCHEMA=L1 .venv/bin/python agree.py --api rules --states 50 --samples 2 --ckpt ...  # dry, no network
  python3 agree.py --selftest                                                            # the arithmetic (no torch)

eval.py --teacher (acceptance A1) asks the teacher once per state on states the student under test
drives, so every model sees different states and one teacher draw per state; at n=60 it is +-10
points and the teacher agrees with itself only ~82%. Here:

  states   --states N unique L1 lines (default 300) from gen_traces.py's sim, deterministic from
           --seed: the lizards are driven by gen_traces.rule_goal (the dry run's rule stand-in), not
           by any student, so every checkpoint and every rerun is scored on the same lines.
  teacher  --samples K answers per state (default 3; the teacher samples at temperature 0.7 as when
           it labelled the traces), cached in out/agree_teacher_cache.jsonl under sha256(sha256 of
           SYSTEM_PROMPT_L1, model, state, sample index): reruns and extra checkpoints ask nothing
           again. Unparsable answers are cached and excluded; transport errors are not cached (a
           rerun asks again). --api rules is rule_goal as the teacher (seeded per state and
           sample: deterministic), for a dry run.
  student  each --ckpt's goal distribution after `<BOS> state ->` (probe_l1.Student: the softmax
           over the seven goal tokens at temperature 1.0, as the device samples it).

Metrics, over the states with >= 1 parsable teacher answer (95% percentile bootstrap CIs over
states, fixed --boot-seed, the same resamples for every checkpoint):

  greedy_agree        mean over (state, teacher answer) of [student argmax == teacher goal]
                      (the A1 quantity, without its noise)
  expected_agree      mean over states of sum_g p_student(g) * p_teacher_hat(g): the chance a goal the
                      device samples matches a teacher sample
  teacher_self_agree  mean over states with >= 2 answers of the share of ordered pairs of distinct
                      answers that agree, sum_g c_g(c_g-1) / (n(n-1)): unbiased for sum_g q(g)^2
                      (the plug-in sum_g p_hat(g)^2 is biased up by sum_g q(g)(1-q(g))/n)
  expected/self       expected_agree / teacher_self_agree, both on the states with >= 2 answers:
                      1.0 is a student that samples as the teacher does (a sharper student that puts
                      its mass on the teacher's mode can exceed 1)
  nll                 mean over (state, teacher answer) of -log p_student(teacher goal) (floor 1e-12)
"""
import argparse
import hashlib
import json
import math
import os
import random
import sys
import tempfile
import threading
import time
import urllib.error
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_traces as gt  # noqa: E402
import train_tokenizer as tok  # noqa: E402

CACHE = os.path.join(HERE, "out", "agree_teacher_cache.jsonl")
P_FLOOR = 1e-12
METRICS = ("greedy_agree", "expected_agree", "teacher_self_agree", "expected/self", "nll")
TRANSPORT = (urllib.error.URLError, TimeoutError, json.JSONDecodeError, KeyError, IndexError, OSError)


# --- states ---------------------------------------------------------------------------------------

def make_states(n, seed, repopulate=50, perturb=0.45):
    """n unique L1 lines from gen_traces' sim, its lizards moved by the rule stand-in (as gen_traces
    --dry-run does, repopulating every `repopulate` lines): no student involved, so the same seed
    gives the same lines. Returns (lines, duplicates skipped)."""
    gt.SCHEMA = "L1"
    rng = random.Random(seed)
    world = gt.new_world(rng)
    gt.warm_up(world)
    lines, seen, dups, idx, repop_at = [], set(), 0, 0, 0
    while len(lines) < n:
        if len(lines) - repop_at >= repopulate and idx % len(gt.members(world)) == 0:
            world = gt.new_world(rng)                  # another pack, keeper, layout and population
            gt.warm_up(world)
            repop_at = len(lines)
        for _ in range(gt.ticks_between(world)):
            world.tick()
        pop = gt.members(world)
        lz = pop[idx % len(pop)]
        idx += 1
        if rng.random() < perturb:
            world.perturb(lz)
        state = gt.encode(world, lz)
        gt.apply_goal(lz, gt.rule_goal(state, rng))
        if state in seen:           # the cache keys on the line: a repeat would share its answers
            dups += 1
            continue
        seen.add(state)
        lines.append(state)
    return lines, dups


# --- teacher --------------------------------------------------------------------------------------

def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def cache_key(prompt_sha, model, state, k):
    return sha(json.dumps([prompt_sha, model, state, k]))


class Cache:
    """append-only JSONL of teacher answers by cache_key; every put is flushed (an interrupted
    run keeps what it paid for)"""

    def __init__(self, path):
        self.path, self.lock, self.rows = path, threading.Lock(), {}
        if os.path.exists(path):
            with open(path) as fh:
                for line in fh:
                    try:
                        rec = json.loads(line)
                    except ValueError:
                        continue                # a line cut by a kill: ignore it
                    self.rows.setdefault(rec.get("key"), rec)

    def get(self, key):
        return self.rows.get(key)

    def put(self, rec):
        with self.lock:
            self.rows.setdefault(rec["key"], rec)
            os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
            with open(self.path, "a") as fh:
                fh.write(json.dumps(rec) + "\n")


def lab_teacher(api, host, model, timeout):
    """(state, k) -> {goal, fail, raw, secs}: gen_traces.teacher_call (what ask_teacher returns the
    goal of), keeping the failure reason; transport errors raise"""
    def ask(state, k):
        r = gt.teacher_call(api, host, model, state, timeout)
        return {"goal": r["goal"], "fail": r["fail"], "raw": r["raw"][:200], "secs": round(r["secs"], 3)}
    return ask


def rules_teacher(state, k):
    """gen_traces.rule_goal as the teacher, its rng seeded by (state, k): the same answers every run"""
    g = gt.rule_goal(state, random.Random(int(sha(f"rules\n{state}\n{k}")[:16], 16)))
    goal, fail = gt.parse_decision(json.dumps({"goal": g.split()[0], "urgency": int(g.split()[2])}))
    return {"goal": goal, "fail": fail, "raw": g, "secs": 0.0}


def collect(states, samples, ask, cache, model, prompt_sha, workers=4, max_requests=None, log=None):
    """answers[i][k]: the cached record for state i's k-th sample, or None (not asked: the cap, a
    transport error, or an abort). Sample 0 of every state is asked before any sample 1, so a capped
    run spreads over the states. Aborts after 5 transport errors with no success."""
    answers = [[None] * samples for _ in states]
    todo, hits = [], 0
    for k in range(samples):
        for i, st in enumerate(states):
            rec = cache.get(cache_key(prompt_sha, model, st, k))
            if rec is not None:
                answers[i][k] = rec
                hits += 1
            else:
                todo.append((i, k))
    capped = todo if max_requests is None else todo[:max(0, max_requests)]
    stats = {"cache_hits": hits, "requests": 0, "transport_errors": 0, "transport_examples": [],
             "aborted": False, "not_asked_cap": len(todo) - len(capped)}
    lock, stop, t0 = threading.Lock(), threading.Event(), time.time()

    def one(job):
        i, k = job
        if stop.is_set():
            return
        try:
            rec = ask(states[i], k)
        except TRANSPORT as e:
            with lock:
                stats["requests"] += 1
                stats["transport_errors"] += 1
                if len(stats["transport_examples"]) < 5:
                    stats["transport_examples"].append(f"{type(e).__name__}: {e}"[:200])
                if stats["transport_errors"] >= 5 and stats["requests"] == stats["transport_errors"]:
                    stats["aborted"] = True
                    stop.set()
            return
        rec = dict(rec, key=cache_key(prompt_sha, model, states[i], k), model=model,
                   prompt_sha=prompt_sha, k=k, state=states[i])
        cache.put(rec)
        answers[i][k] = rec
        with lock:
            stats["requests"] += 1
            n = stats["requests"]
        if log and (n % 25 == 0 or n == len(capped)):
            rate = n / max(1e-9, time.time() - t0)
            log(f"  teacher {n}/{len(capped)} ({rate:.2f}/s, ~{(len(capped) - n) / rate / 60:.1f} min left)")

    if capped:
        with ThreadPoolExecutor(max_workers=max(1, workers)) as ex:
            list(ex.map(one, capped))
    return answers, stats


def teacher_goals(answers):
    """per state, the parsable goals (goal names only); and the count of unparsable answers by reason"""
    goals, fails = [], {}
    for row in answers:
        gs = []
        for rec in row:
            if rec is None:
                continue
            if rec.get("goal"):
                gs.append(rec["goal"].split()[0])
            else:
                fails[rec.get("fail") or "unknown"] = fails.get(rec.get("fail") or "unknown", 0) + 1
        goals.append(gs)
    return goals, fails


# --- metrics --------------------------------------------------------------------------------------

def pair_agree(goals):
    """the share of ordered pairs of distinct answers that agree (None under 2 answers): unbiased
    for the teacher's sum_g q(g)^2"""
    n = len(goals)
    if n < 2:
        return None
    counts = {}
    for g in goals:
        counts[g] = counts.get(g, 0) + 1
    return sum(c * (c - 1) for c in counts.values()) / (n * (n - 1))


def state_stats(p, goals):
    """one state's sufficient statistics: (answers, argmax hits, expected agreement, nll sum,
    pair agreement or None)"""
    top = max(gt.GOALS_L1, key=lambda g: p[g])         # ties: the first in GOALS_L1
    n = len(goals)
    return (n, sum(g == top for g in goals), sum(p[g] for g in goals) / n,
            sum(-math.log(max(p[g], P_FLOOR)) for g in goals), pair_agree(goals))


def metrics(stats):
    """the metrics of a list of state_stats (a bootstrap resample is just another list)"""
    answers = sum(s[0] for s in stats)
    two = [s for s in stats if s[4] is not None]
    self_agree = sum(s[4] for s in two) / len(two) if two else None
    exp_two = sum(s[2] for s in two) / len(two) if two else None
    return {"greedy_agree": sum(s[1] for s in stats) / answers,
            "expected_agree": sum(s[2] for s in stats) / len(stats),
            "teacher_self_agree": self_agree,
            "expected/self": exp_two / self_agree if self_agree else None,
            "nll": sum(s[3] for s in stats) / answers}


def percentile(sorted_vals, q):
    """linear interpolation between order statistics (numpy's default)"""
    x = q * (len(sorted_vals) - 1)
    lo = int(math.floor(x))
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (x - lo)


def bootstrap(stats, boot, seed):
    """{metric: (lo, hi)}: the 95% percentile interval over `boot` resamples of the states"""
    rng, n = random.Random(seed), len(stats)
    draws = {m: [] for m in METRICS}
    for _ in range(boot):
        res = metrics([stats[rng.randrange(n)] for _ in range(n)])
        for m in METRICS:
            if res[m] is not None:
                draws[m].append(res[m])
    out = {}
    for m, vals in draws.items():
        vals.sort()
        out[m] = (percentile(vals, 0.025), percentile(vals, 0.975)) if vals else (None, None)
    return out


def score(dists, goals, boot, boot_seed):
    """one checkpoint: {metric: {value, ci, n}} over the states with >= 1 answer"""
    stats = [state_stats(p, gs) for p, gs in zip(dists, goals) if gs]
    if not stats:
        return None
    point, ci = metrics(stats), bootstrap(stats, boot, boot_seed)
    n_ans, n_two = sum(s[0] for s in stats), sum(s[4] is not None for s in stats)
    n = {"greedy_agree": n_ans, "nll": n_ans, "expected_agree": len(stats),
         "teacher_self_agree": n_two, "expected/self": n_two}
    return {m: {"value": point[m], "ci": list(ci[m]), "n": n[m]} for m in METRICS}


def fmt(v, m):
    return "    -" if v is None else f"{v:.3f}" if m != "nll" else f"{v:.4f}"


def print_table(res):
    print(f"  {'metric':20s} {'value':>7s}   {'95% CI':19s} n")
    for m in METRICS:
        r = res[m]
        lo, hi = r["ci"]
        ci = f"[{fmt(lo, m)}, {fmt(hi, m)}]"
        print(f"  {m:20s} {fmt(r['value'], m):>7s}   {ci:19s} {r['n']}")


def ckpt_name(path):
    """the JSON's name: the file's stem, or for harness.py's <run>/<seed>/ckpt.pt `<run>_<seed>`"""
    stem = os.path.splitext(os.path.basename(path))[0]
    if stem != "ckpt":
        return stem
    parts = os.path.normpath(os.path.abspath(path)).split(os.sep)
    return "_".join(parts[-3:-1])


# --- selftest -------------------------------------------------------------------------------------

def selftest():
    """the arithmetic on a fake teacher and a fake student with known distributions"""
    goals = gt.GOALS_L1
    gt.SCHEMA = "L1"

    def mk(**kw):
        rest = (1.0 - sum(kw.values())) / (len(goals) - len(kw))
        return {g: kw.get(g, rest) for g in goals}

    # the teacher's raw replies per state and sample: parsed by gen_traces.parse_decision as the lab's
    ok = lambda g: json.dumps({"goal": g, "urgency": 5})  # noqa: E731
    bad_json, bad_goal = "{not json", json.dumps({"goal": "swim", "urgency": 3})
    raw = {"sA": [ok("hunt"), ok("hunt"), ok("bask"), ok("hunt")],
           "sB": [ok("hide"), ok("hide"), ok("hide"), ok("hide")],
           "sC": [ok("rest"), bad_json, ok("explore"), ok("rest")],
           "sD": [ok("dig"), bad_goal, bad_json, ok("dig")],
           "sE": [bad_json, bad_goal, bad_json, ok("hide")],
           "sF": [ok("hunt"), ok("hunt"), ok("hunt"), ok("hunt")]}   # (sF k=1: a transport error, once)
    states = list(raw)
    calls, broke = [], set()

    def fake_teacher(state, k):
        calls.append((state, k))
        if (state, k) == ("sF", 1) and (state, k) not in broke:
            broke.add((state, k))
            raise OSError("connection reset (fake)")
        goal, fail = gt.parse_decision(raw[state][k])
        return {"goal": goal, "fail": fail, "raw": raw[state][k], "secs": 0.0}

    student = {"sA": mk(hunt=0.6, bask=0.3), "sB": mk(rest=0.5, hide=0.4), "sC": mk(explore=0.7),
               "sD": mk(dig=0.4), "sE": mk(hide=0.9), "sF": mk(hunt=0.9)}
    dists = [student[s] for s in states]
    psha = sha(gt.SYSTEM_PROMPT_L1)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "cache.jsonl")
        # run 1: 18 asks, one transport error (not cached), six unparsable answers (cached)
        answers, st = collect(states, 3, fake_teacher, Cache(path), "fake", psha, workers=3)
        assert len(calls) == 18 and st["requests"] == 18 and st["cache_hits"] == 0, st
        assert st["transport_errors"] == 1 and not st["aborted"] and answers[5][1] is None, st
        tg, fails = teacher_goals(answers)
        assert tg == [["hunt", "hunt", "bask"], ["hide"] * 3, ["rest", "explore"], ["dig"], [], ["hunt", "hunt"]], tg
        assert fails == {"bad_json": 4, "bad_goal": 2}, fails   # sC1, sD2, sE0, sE2 bad_json; sD1, sE1 bad_goal
        assert sum(1 for line in open(path)) == 17
        res = score(dists, tg, 500, 1)
        # by hand: argmax A hunt (2 hits), B rest (0), C explore (1), D dig (1), F hunt (2): 6 of 11
        # expected: A (.6+.6+.3)/3, B .4, C (.05+.7)/2, D .4, F .9 over 5 states
        # self (>= 2 answers: A, B, C, F): A 2*1/(3*2), B 1, C 0, F 1
        # expected/self on A, B, C, F: ((.5+.4+.375+.9)/4) / (7/12)
        nll = -(2 * math.log(.6) + math.log(.3) + 3 * math.log(.4) + math.log(.05) + math.log(.7)
                + math.log(.4) + 2 * math.log(.9)) / 11
        want = {"greedy_agree": (6 / 11, 11), "expected_agree": (0.515, 5), "teacher_self_agree": (7 / 12, 4),
                "expected/self": (0.54375 / (7 / 12), 4), "nll": (nll, 11)}
        for m, (v, n) in want.items():
            r = res[m]
            assert abs(r["value"] - v) < 1e-12 and r["n"] == n, (m, r, v, n)
            assert r["ci"][0] <= r["value"] + 1e-12 and r["value"] - 1e-12 <= r["ci"][1], (m, r)
        assert score(dists, tg, 500, 1) == res, "the bootstrap is not deterministic"
        print("  run 1 (fake teacher, K=3):")
        print_table(res)

        # run 2, a fresh process's view: only the transport error is asked again
        calls.clear()
        answers, st = collect(states, 3, fake_teacher, Cache(path), "fake", psha, workers=2)
        assert calls == [("sF", 1)] and st["cache_hits"] == 17 and st["requests"] == 1, (calls, st)
        tg, _ = teacher_goals(answers)
        res2 = score(dists, tg, 500, 1)
        assert abs(res2["greedy_agree"]["value"] - 7 / 12) < 1e-12 and res2["greedy_agree"]["n"] == 12
        assert abs(res2["expected_agree"]["value"] - 0.515) < 1e-12                  # F's mean is still .9
        # run 3: K=4 asks only the fourth sample; another model or prompt misses everything; a cap
        calls.clear()
        answers, st = collect(states, 4, fake_teacher, Cache(path), "fake", psha)
        assert sorted(calls) == [(s, 3) for s in states] and st["cache_hits"] == 18, (calls, st)
        assert teacher_goals(answers)[0][4] == ["hide"]                           # sE now counts
        calls.clear()
        _, st = collect(states, 4, fake_teacher, Cache(path), "other", psha, max_requests=5)
        assert len(calls) == 5 and st["cache_hits"] == 0 and st["not_asked_cap"] == 19, st
        _, st = collect(states, 4, fake_teacher, Cache(path), "fake", sha("another prompt"), max_requests=0)
        assert st["cache_hits"] == 0 and st["requests"] == 0 and st["not_asked_cap"] == 24, st
        # a run whose every request fails stops after 5
        calls.clear()

        def down(state, k):
            calls.append((state, k))
            raise urllib.error.URLError("refused (fake)")
        _, st = collect(states, 4, down, Cache(path), "down", psha, workers=1)
        assert st["aborted"] and st["transport_errors"] == 5 and len(calls) == 5, st

        # the JSON round trip
        out = os.path.join(tmp, "agree.json")
        with open(out, "w") as fh:
            json.dump({"checkpoints": [{"metrics": res}]}, fh)
        assert json.load(open(out))["checkpoints"][0]["metrics"] == json.loads(json.dumps(res))

    # unbiasedness, exactly over every K=3 draw from q: E[pair_agree] = sum q^2, the plug-in is biased,
    # and E[mean_j p(a_j)] = sum_g p(g) q(g)
    q, p = {"hunt": 0.5, "bask": 1 / 3, "rest": 1 / 6}, mk(hunt=0.2, bask=0.5, rest=0.1)
    e_pair = e_plug = e_exp = 0.0
    for a in q:
        for b in q:
            for c in q:
                w, draw = q[a] * q[b] * q[c], [a, b, c]
                e_pair += w * pair_agree(draw)
                e_plug += w * sum((draw.count(g) / 3) ** 2 for g in q)
                e_exp += w * state_stats(p, draw)[2]
    sq = sum(v * v for v in q.values())
    assert abs(e_pair - sq) < 1e-12 and abs(sq - 7 / 18) < 1e-12, (e_pair, sq)
    assert abs(e_plug - (sq + sum(v * (1 - v) for v in q.values()) / 3)) < 1e-12 and e_plug > sq + 0.1
    assert abs(e_exp - sum(p[g] * q[g] for g in q)) < 1e-12
    assert pair_agree(["hunt"]) is None and pair_agree(["dig", "dig"]) == 1.0

    # a bootstrap of identical states has no spread
    flat = score([mk(hunt=0.6)] * 7, [["hunt", "bask"]] * 7, 200, 3)
    assert all(r["ci"][0] == r["ci"][1] == r["value"] for r in flat.values()), flat
    assert abs(flat["teacher_self_agree"]["value"]) < 1e-12 and flat["expected/self"]["value"] is None

    # the states and the rules teacher are deterministic, and the lines are L1 lines
    a, _ = make_states(12, 5)
    b, _ = make_states(12, 5)
    assert a == b and len(set(a)) == 12 and not any(gt.check_l1_line(s) for s in a), a
    assert make_states(12, 6)[0] != a
    assert [rules_teacher(s, k) for s in a for k in range(3)] == [rules_teacher(s, k) for s in a for k in range(3)]
    assert ckpt_name("out/harness/run-x/s1/ckpt.pt") == "run-x_s1" and ckpt_name("out/ckpt_l1.pt") == "ckpt_l1"
    print("\nselftest OK: greedy 6/11, expected 0.515, self 7/12 (unbiased: E = sum q^2 = 7/18 exactly), "
          "expected/self 0.932, nll; cache hits/misses/caps/abort; deterministic states and bootstrap")


# --- main -----------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ckpt", action="extend", nargs="+", default=[],
                    help="train.py checkpoint(s) (POCKET_SCHEMA=L1); repeat or list several")
    ap.add_argument("--states", type=int, default=300, help="unique state lines")
    ap.add_argument("--seed", type=int, default=777, help="the sim's seed (the state set)")
    ap.add_argument("--repopulate", type=int, default=50, help="a new terrarium every N lines")
    ap.add_argument("--samples", type=int, default=3, help="teacher answers per state (K)")
    ap.add_argument("--api", choices=("openai", "ollama", "rules"), default="openai",
                    help=f"teacher: the lab (key from ${gt.LAB_KEY_ENV}), Ollama, or rule_goal (no network)")
    ap.add_argument("--host", default=None, help="teacher base URL (default per --api, as gen_traces)")
    ap.add_argument("--model", default=None, help="teacher model (default per --api, as gen_traces)")
    ap.add_argument("--timeout", type=float, default=30.0, help="per-request timeout, seconds")
    ap.add_argument("--workers", type=int, default=4, help="concurrent teacher requests")
    ap.add_argument("--max-requests", type=int, default=None, help="cap on teacher requests this run")
    ap.add_argument("--cache", default=CACHE, help="teacher answer cache (JSONL)")
    ap.add_argument("--boot", type=int, default=2000, help="bootstrap resamples")
    ap.add_argument("--boot-seed", type=int, default=12345)
    ap.add_argument("--device", default="cpu", help="cpu|cuda: where the checkpoints run")
    ap.add_argument("--out", default=None, help="the JSON (default out/agree_<first ckpt name>.json)")
    ap.add_argument("--selftest", action="store_true", help="check the arithmetic on fakes and stop")
    args = ap.parse_args()
    tok.set_schema("L1")
    gt.SCHEMA = "L1"
    if args.selftest:
        selftest()
        return
    if not args.ckpt:
        ap.error("--ckpt is required (or --selftest)")
    if args.samples < 1 or args.states < 1:
        ap.error("--samples and --states must be >= 1")
    for c in args.ckpt:
        if not os.path.exists(c):
            sys.exit(f"missing {c}")
    host, model = gt.teacher_defaults(args.api, args.host, args.model)
    if args.api == "rules":
        model, ask = args.model or "rules", rules_teacher
    else:
        ask = lab_teacher(args.api, host, model, args.timeout)
    psha = sha(gt.SYSTEM_PROMPT_L1)
    log = lambda m: print(m, file=sys.stderr, flush=True)  # noqa: E731

    t0 = time.time()
    states, dups = make_states(args.states, args.seed, args.repopulate)
    states_sha = sha("\n".join(states))
    log(f"{len(states)} states (seed {args.seed}, {dups} repeats skipped, sha {states_sha[:12]}) "
        f"in {time.time() - t0:.1f}s")

    cache = Cache(args.cache)
    answers, tstats = collect(states, args.samples, ask, cache, model, psha, args.workers, args.max_requests, log)
    goals, fails = teacher_goals(answers)
    answered = sum(1 for g in goals if g)
    n_answers = sum(len(g) for g in goals)
    not_asked = sum(rec is None for row in answers for rec in row)
    mix = {}
    for gs in goals:
        for g in gs:
            mix[g] = mix.get(g, 0) + 1
    print(f"teacher {args.api}:{model}, K={args.samples}: {n_answers} answers on {answered}/{len(states)} states; "
          f"cache hits {tstats['cache_hits']}, requests {tstats['requests']}, "
          f"unparsable {sum(fails.values())}{' ' + str(fails) if fails else ''}, "
          f"transport errors {tstats['transport_errors']}, "
          f"not answered {not_asked}" + (" (ABORTED: teacher unreachable)" if tstats["aborted"] else ""))
    for e in tstats["transport_examples"]:
        print(f"  [transport] {e}")
    if n_answers:
        print("  teacher goal mix: " + " ".join(f"{g} {mix.get(g, 0) / n_answers:.2f}" for g in gt.GOALS_L1))
    if not answered:
        sys.exit("no teacher answers: nothing to score")

    import probe_l1                          # torch from here on
    results = []
    for c in args.ckpt:
        student = probe_l1.Student(c, 1.0, args.device)
        out = student.dists(states)
        dists, masses = [d for d, _ in out], [m for _, m in out]
        res = score(dists, goals, args.boot, args.boot_seed)
        argmax_mix = {}
        for d, gs in zip(dists, goals):
            if gs:
                top = max(gt.GOALS_L1, key=lambda g: d[g])
                argmax_mix[top] = argmax_mix.get(top, 0) + 1
        print(f"\n{c}: iter {student.info['iter']}, val_loss {student.info['val_loss']}; "
              f"goal-token mass mean {sum(masses) / len(masses):.3f}, min {min(masses):.3f}")
        print_table(res)
        print("  student argmax mix: " + " ".join(f"{g} {argmax_mix.get(g, 0) / answered:.2f}" for g in gt.GOALS_L1))
        results.append({"ckpt": c, "checkpoint": student.info, "metrics": res,
                        "goal_mass_mean": sum(masses) / len(masses), "goal_mass_min": min(masses),
                        "argmax_mix": argmax_mix})
        del student

    path = args.out or os.path.join(HERE, "out", f"agree_{ckpt_name(args.ckpt[0])}.json")
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    rep = {"settings": {"seed": args.seed, "states": args.states, "repopulate": args.repopulate, "perturb": 0.45,
                        "samples": args.samples, "api": args.api, "host": host if args.api != "rules" else None,
                        "model": model, "prompt_sha256": psha, "temperature_student": 1.0,
                        "boot": args.boot, "boot_seed": args.boot_seed, "device": args.device,
                        "cache": args.cache, "max_requests": args.max_requests},
           "states_sha256": states_sha, "n_states": answered, "n_states_generated": len(states),
           "duplicates_skipped": dups, "n_answers": n_answers,
           "failures": {"unparsable": fails, "transport": tstats["transport_errors"], "not_answered": not_asked,
                        "aborted": tstats["aborted"]},
           "teacher": {"cache_hits": tstats["cache_hits"], "requests": tstats["requests"],
                       "goal_mix": {g: mix.get(g, 0) / n_answers for g in gt.GOALS_L1}},
           "checkpoints": results}
    with open(path, "w") as fh:
        json.dump(rep, fh, indent=1)
    print(f"\n-> {path}")


if __name__ == "__main__":
    main()

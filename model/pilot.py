#!/usr/bin/env python3
"""Pilot a teacher on hand-written L1 edge states (PLAN.md P4 "Pilot"). Stdlib only.

  python3 model/pilot.py --api openai --model chat-teacher   # k=3 asks per state, 4 workers
  python3 model/pilot.py --api rules                         # gen_traces.rule_goal: checks the expectations, no network
  python3 model/pilot.py --validate                          # only check pilot_l1.jsonl
  python3 model/pilot.py --api openai --prompt-file cand.txt # a candidate SYSTEM_PROMPT_L1

The key for --api openai comes from $LAB_API_KEY only (gen_traces.openai_raw).

pilot_l1.jsonl, one state per line: id; probe (the acceptance row of
model/schema-lizard.md it stands for, or the rule); rules (the "Behaviour the
teacher must express" rules it exercises); accept (the goals a teacher that
follows the rules may give); state (an L1 line); why.

Reports per-state agreement (the share of a state's valid answers inside
`accept`), overall agreement, agreement per rule and per probe, JSON failures
(a reply that is not JSON, a goal outside the enum, transport errors),
decisions per minute at the given worker count, latency, prompt tokens, a goal
histogram and the five worst states with every answer. The whole run goes to
out/pilot_<model>_<time>.json (the lab's MLflow can take it from there).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_traces as gt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def load_states(path):
    states, problems, ids = [], [], set()
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            if not line.strip():
                continue
            s = json.loads(line)
            for k in ("id", "probe", "rules", "accept", "state"):
                if k not in s:
                    problems.append(f"line {n}: no {k!r}")
            if s.get("id") in ids:
                problems.append(f"line {n}: duplicate id {s['id']}")
            ids.add(s.get("id"))
            problems += [f"{s.get('id')}: {p}" for p in gt.check_l1_line(s.get("state", ""))]
            bad = [g for g in s.get("accept", []) if g not in gt.GOALS_L1]
            if bad or not s.get("accept"):
                problems.append(f"{s.get('id')}: accept {s.get('accept')} (goals are {gt.GOALS_L1})")
            if not set(s.get("rules", [])) <= set(range(1, 10)):
                problems.append(f"{s.get('id')}: rules {s.get('rules')} (1-9)")
            states.append(s)
    return states, problems


def pct(a, b):
    return f"{100 * a / b:5.1f}%" if b else "   n/a"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--api", choices=("openai", "ollama", "rules"), default="openai")
    ap.add_argument("--host", default=None)
    ap.add_argument("--model", default=None)
    ap.add_argument("--states", default=os.path.join(HERE, "pilot_l1.jsonl"))
    ap.add_argument("--k", type=int, default=3, help="asks per state")
    ap.add_argument("--workers", type=int, default=4, help="parallel requests (chat-teacher has 4 slots)")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--prompt-file", default=None, help="a candidate system prompt (default: SYSTEM_PROMPT_L1)")
    ap.add_argument("--only", default=None, help="comma-separated state ids (a re-check of a few)")
    ap.add_argument("--max-requests", type=int, default=150, help="refuse a run that would ask more than this")
    ap.add_argument("--validate", action="store_true", help="check the states file and stop")
    ap.add_argument("--out", default=None, help="results JSON (default out/pilot_<model>_<time>.json)")
    args = ap.parse_args()
    gt.SCHEMA = "L1"

    states, problems = load_states(args.states)
    if problems:
        sys.exit("pilot states are not valid L1:\n  " + "\n  ".join(problems))
    if args.only:
        keep = set(args.only.split(","))
        states = [s for s in states if s["id"] in keep]
    print(f"{len(states)} states OK ({args.states})")
    if args.validate:
        return
    if args.prompt_file:
        gt.SYSTEM_PROMPT_L1 = open(args.prompt_file).read().strip()
    prompt = gt.system_prompt()
    host, model = gt.teacher_defaults(args.api, args.host, args.model)
    model = model or "rules"
    jobs = [s for s in states for _ in range(args.k)]
    if len(jobs) > args.max_requests:
        sys.exit(f"{len(jobs)} requests > --max-requests {args.max_requests}")
    if args.api == "openai" and not os.environ.get(gt.LAB_KEY_ENV):
        print(f"[warn] ${gt.LAB_KEY_ENV} is not set: no Authorization header", file=sys.stderr)
    print(f"{len(jobs)} asks to {model} @ {host or 'local'} ({args.workers} workers); "
          f"system prompt {len(prompt)} chars, {len(prompt.split())} words, "
          f"sha1 {hashlib.sha1(prompt.encode()).hexdigest()[:10]}", flush=True)

    def ask(s):
        try:
            r = gt.teacher_call(args.api, host, model, s["state"], args.timeout)
        except Exception as e:  # noqa: BLE001 - a transport failure is a result here
            r = {"goal": None, "fail": f"transport:{type(e).__name__}", "raw": str(e)[:200], "usage": {}, "secs": 0.0}
        return s["id"], r

    t0 = time.time()
    answers = {s["id"]: [] for s in states}
    with cf.ThreadPoolExecutor(max(1, args.workers)) as ex:
        for sid, r in ex.map(ask, jobs):
            answers[sid].append(r)
    wall = time.time() - t0

    # ---- scoring
    rows, by_rule, by_probe, hist, urg = [], {}, {}, {}, {}
    fails = {}
    agree_n = valid_n = 0
    for s in states:
        rs = answers[s["id"]]
        goals = [r["goal"].split()[0] for r in rs if r["goal"]]
        for r in rs:
            if r["fail"]:
                fails[r["fail"].split(":")[0]] = fails.get(r["fail"].split(":")[0], 0) + 1
            if r["goal"]:
                g, u = r["goal"].split()[0], int(r["goal"].split()[2])
                hist[g] = hist.get(g, 0) + 1
                urg.setdefault(g, []).append(u)
        ok = sum(1 for g in goals if g in s["accept"])
        agree_n += ok
        valid_n += len(goals)
        rows.append((ok / len(goals) if goals else 0.0, s, rs, ok, len(goals)))
        for key, bucket in [(f"rule {k}", by_rule) for k in s["rules"]] + [(s["probe"], by_probe)]:
            a, n = bucket.get(key, (0, 0))
            bucket[key] = (a + ok, n + len(goals))
    secs = sorted(r["secs"] for rs in answers.values() for r in rs if r["goal"] is not None or r["fail"])
    ptok = [r["usage"].get("prompt_tokens") for rs in answers.values() for r in rs if r["usage"].get("prompt_tokens")]
    ctok = [r["usage"].get("completion_tokens") for rs in answers.values() for r in rs
            if r["usage"].get("completion_tokens")]

    # ---- report
    print("\nper state (agreement = answers inside `accept`; worst first):")
    for frac, s, rs, ok, n in sorted(rows, key=lambda t: (t[0], t[1]["id"])):
        got = ", ".join(r["goal"] or r["fail"] for r in rs)
        print(f"  {ok}/{n}  {s['id']:28s} want {'|'.join(s['accept']):34s} got {got}")
    print(f"\noverall agreement: {agree_n}/{valid_n} = {pct(agree_n, valid_n).strip()}"
          f"  (states fully agreed: {sum(1 for t in rows if t[4] and t[3] == t[4])}/{len(rows)})")
    print("by rule:  " + "  ".join(f"{k} {pct(*by_rule[k]).strip()} (n={by_rule[k][1]})"
                                   for k in sorted(by_rule, key=lambda x: int(x.split()[1]))))
    print("by probe: " + "  ".join(f"{k} {pct(*v).strip()}" for k, v in sorted(by_probe.items())))
    n_ask = len(jobs)
    print(f"failures: {sum(fails.values())}/{n_ask} " + (str(fails) if fails else "(none)")
          + "  [bad_json = not JSON, bad_goal = goal outside the enum, transport = no reply]")
    if secs:
        print(f"rate: {60 * valid_n / wall:.1f} decisions/min with {args.workers} workers over {wall:.1f} s; "
              f"latency median {secs[len(secs) // 2]:.2f} s, p90 {secs[int(len(secs) * 0.9)]:.2f} s")
    if ptok:
        print(f"tokens: prompt {min(ptok)}-{max(ptok)}, answer {min(ctok) if ctok else '?'}-{max(ctok) if ctok else '?'}"
              f" -> largest request {max(ptok) + (max(ctok) if ctok else 0)} of the 4K slot")
    total = sum(hist.values()) or 1
    print("goals:    " + "  ".join(f"{g} {100 * c / total:.0f}%" for g, c in sorted(hist.items(), key=lambda kv: -kv[1])))
    print("urgency:  " + "  ".join(f"{g} {sum(v) / len(v):.1f}" for g, v in sorted(urg.items())))
    print("\nworst five:")
    for frac, s, rs, ok, n in sorted(rows, key=lambda t: (t[0], t[1]["id"]))[:5]:
        print(f"  {s['id']} ({s['probe']}; want {'|'.join(s['accept'])}): {s['why']}")
        print(f"    {s['state']}")
        print(f"    -> {', '.join(r['goal'] or (r['fail'] + ' ' + r['raw'][:60]) for r in rs)}")

    out = args.out or os.path.join(HERE, "out", "pilot_{}_{}.json".format(
        re.sub(r"[^A-Za-z0-9_.-]+", "-", model), time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fh:
        json.dump({"api": args.api, "host": host, "model": model, "k": args.k, "workers": args.workers,
                   "prompt_sha1": hashlib.sha1(prompt.encode()).hexdigest(), "prompt": prompt,
                   "wall_s": wall, "agree": agree_n, "valid": valid_n, "asks": n_ask, "fails": fails,
                   "by_rule": by_rule, "by_probe": by_probe, "goals": hist,
                   "prompt_tokens_max": max(ptok) if ptok else None,
                   "states": [{"id": s["id"], "probe": s["probe"], "accept": s["accept"], "state": s["state"],
                               "answers": [{k: r[k] for k in ("goal", "fail", "raw", "usage", "secs")}
                                           for r in answers[s["id"]]]} for s in states]}, fh, indent=1)
    print(f"\n-> {out}")


if __name__ == "__main__":
    main()

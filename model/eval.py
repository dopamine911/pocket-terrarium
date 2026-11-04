#!/usr/bin/env python3
"""Evaluate the exported student model against fresh sim states. Stdlib only.

Samples never-seen states from the same headless sim as gen_traces.py, runs the
native llama2.c binary on each, and reports what the student decided. With
--teacher it also asks the teacher for its goal on the same states and reports
student/teacher agreement (top-line distillation metric; L1's acceptance A1).

  python3 eval.py --count 20                 # student only, prints a table
  python3 eval.py --count 50 --teacher       # adds gemma4:26b agreement
  python3 eval.py --schema L1 --count 60 --teacher --api openai \\
      --model-bin out/model_l1.bin --tok-bin out/tokenizer_l1.bin   # the lizard line vs chat-teacher
"""

import argparse
import os
import random
import re
import subprocess
import sys

import gen_traces as gt

HERE = os.path.dirname(os.path.abspath(__file__))


def student_goal(run_bin, model_bin, tok_bin, state):
    prompt = state + " ->"
    out = subprocess.run(
        [run_bin, model_bin, "-z", tok_bin, "-t", "0", "-i", prompt],
        capture_output=True, text=True, timeout=60).stdout
    completion = out.split("->", 1)[1] if "->" in out else out
    m = re.search(r"([a-z_]+) urgency (\d)", completion)
    return f"{m.group(1)} urgency {m.group(2)}" if m else f"<unparsed: {completion.strip()[:40]}>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--teacher", action="store_true", help="also ask the teacher and score agreement")
    ap.add_argument("--api", choices=("ollama", "openai"), default="ollama",
                    help=f"teacher API (openai: the lab, key from ${gt.LAB_KEY_ENV})")
    ap.add_argument("--host", default=None, help="teacher base URL (default per --api, as gen_traces)")
    ap.add_argument("--model", default=None, help="teacher model (default per --api, as gen_traces)")
    ap.add_argument("--run-bin", default=os.path.join(HERE, "runw"))
    ap.add_argument("--model-bin", default=os.path.join(HERE, "out", "model.bin"))
    ap.add_argument("--tok-bin", default=os.path.join(HERE, "out", "tokenizer.bin"))
    ap.add_argument("--schema", type=gt.parse_schema, choices=(2, 3, 4, "L1"), default=2,
                    help="state line schema of the model under test")
    args = ap.parse_args()
    gt.SCHEMA = args.schema
    host, model = gt.teacher_defaults(args.api, args.host, args.model)

    for p in (args.run_bin, args.model_bin, args.tok_bin):
        if not os.path.exists(p):
            sys.exit(f"missing {p} (build/train/export first)")

    rng = random.Random(args.seed)
    world = gt.new_world(rng)
    gt.warm_up(world)

    agree = valid = 0
    for i in range(args.count):
        for _ in range(gt.ticks_between(world)):
            world.tick()
        pop = gt.members(world)
        agent = pop[i % len(pop)]
        if rng.random() < 0.45:
            world.perturb(agent)
        state = gt.encode(world, agent)
        s_goal = student_goal(args.run_bin, args.model_bin, args.tok_bin, state)
        ok = not s_goal.startswith("<")
        valid += ok
        line = f"[{i+1:3d}] {s_goal:28s}"
        if args.teacher:
            t_goal = gt.ask_teacher(args.api, host, model, state, 30) or "<teacher failed>"
            hit = s_goal.split()[0] == t_goal.split()[0]
            agree += hit
            line += f" teacher: {t_goal:28s} {'MATCH' if hit else 'diff'}"
        print(line + f"  | {state}")
        if ok and not gt.is_l1():
            agent.goal = s_goal.split()[0]
        elif ok and s_goal.split()[0] in gt.GOALS_L1:
            gt.apply_goal(agent, s_goal)        # a lizard also takes the urgency

    print(f"\nvalid output: {valid}/{args.count}")
    if args.teacher:
        print(f"goal agreement with teacher: {agree}/{args.count} ({100*agree/args.count:.0f}%)")


if __name__ == "__main__":
    main()

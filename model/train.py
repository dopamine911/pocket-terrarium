#!/usr/bin/env python3
"""Train the pocket-tank advisor on (state, goal) traces.

A minimal single-device training loop around llama2.c's Transformer (imported from
./llama2.c/model.py), replacing its tinystories-specific train.py. Data is every
model/out/traces*.jsonl line, serialized per train_tokenizer.py (BOS-separated
"state -> goal" docs, char-level vocab 355).

  python3 train.py                          # tiny proof model: dim 128, 4 layers
  python3 train.py --dim 288 --n-layers 6   # scale up once traces are plentiful

Checkpoints are export.py-compatible:
  python3 llama2.c/export.py out/model.bin --checkpoint out/ckpt.pt
  ./llama2.c/run out/model.bin -z out/tokenizer.bin -i "<state line> ->"

Requires torch (see README / venv). Everything else is stdlib.
"""

import argparse
import glob
import json
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "llama2.c"))

import torch  # noqa: E402
from model import ModelArgs, Transformer  # noqa: E402  (llama2.c/model.py)

import train_tokenizer as tok  # noqa: E402


def load_streams(data_glob, val_frac, rng):
    docs = []
    for path in sorted(glob.glob(data_glob)):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                docs.append(tok.encode_doc(obj["state"], obj["goal"]))
    if len(docs) < 50:
        sys.exit(f"only {len(docs)} docs matched {data_glob}; need more traces")
    rng.shuffle(docs)
    n_val = max(1, int(len(docs) * val_frac))
    flat = lambda ds: torch.tensor([t for d in ds for t in d], dtype=torch.long)
    return flat(docs[n_val:]), flat(docs[:n_val]), docs[n_val:], docs[:n_val]


def decision_tensors(docs, seq_len):
    """one row per doc from its BOS: inputs, targets only on the decision (the tokens after `->`:
    goal, " urgency", digit - what the device reads), and the `->` position"""
    sep = tok.encode("->")[0]
    width = max(len(d) for d in docs) - 1
    if width > seq_len:
        sys.exit(f"a doc of {width + 1} tokens does not fit --max-seq-len {seq_len}")
    x = torch.zeros(len(docs), width, dtype=torch.long)
    y = torch.full((len(docs), width), -1, dtype=torch.long)
    pos = torch.empty(len(docs), dtype=torch.long)
    for i, d in enumerate(docs):
        j = d.index(sep)
        x[i, :len(d) - 1] = torch.tensor(d[:-1])
        y[i, j:len(d) - 1] = torch.tensor(d[j + 1:])
        pos[i] = j
    return x, y, pos


@torch.no_grad()
def goal_metrics(model, dt, goal_ids, device, batch=512):
    """on whole val docs: the goal's NLL under the device's softmax over the goal tokens only,
    how often its top goal is the label, and the full-vocab loss on the decision tokens"""
    x, y, pos = dt
    gid = torch.tensor(goal_ids, device=device)
    model.eval()
    nll = hits = dec = 0.0
    for k in range(0, len(x), batch):
        xb, yb, pb = x[k:k + batch].to(device), y[k:k + batch].to(device), pos[k:k + batch].to(device)
        logits = model(xb, yb)
        dec += model.last_loss.item() * len(xb)
        rows = torch.arange(len(xb), device=device)
        lp = torch.log_softmax(logits[rows, pb][:, gid], dim=-1)
        label = (yb[rows, pb].unsqueeze(1) == gid).float().argmax(dim=1)
        nll -= lp[rows, label].sum().item()
        hits += (lp.argmax(dim=1) == label).sum().item()
    model.train()
    return nll / len(x), hits / len(x), dec / len(x)


def get_batch(stream, batch, seq_len, device, rng):
    ix = [rng.randint(0, len(stream) - seq_len - 2) for _ in range(batch)]
    x = torch.stack([stream[i:i + seq_len] for i in ix])
    y = torch.stack([stream[i + 1:i + seq_len + 1] for i in ix])
    return x.to(device), y.to(device)


def get_decision_batch(dt, batch, device, rng):
    ix = torch.tensor([rng.randrange(len(dt[0])) for _ in range(batch)])
    return dt[0][ix].to(device), dt[1][ix].to(device)


@torch.no_grad()
def eval_loss(model, stream, batch, seq_len, device, rng, iters=20):
    model.eval()
    losses = []
    for _ in range(iters):
        x, y = get_batch(stream, batch, seq_len, device, rng)
        model(x, y)
        losses.append(model.last_loss.item())
    model.train()
    return sum(losses) / len(losses)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(here, "out", "traces*.jsonl"))
    ap.add_argument("--out", default=os.path.join(here, "out", "ckpt.pt"))
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--n-layers", type=int, default=4)
    ap.add_argument("--n-heads", type=int, default=4)
    ap.add_argument("--max-seq-len", type=int, default=256)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--iters", type=int, default=2000)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--warmup", type=int, default=100)
    ap.add_argument("--eval-every", type=int, default=200)
    ap.add_argument("--val-frac", type=float, default=0.05)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--loss", choices=("all", "decision"), default="all",
                    help="all: every token of random windows (the shipped models); decision: whole docs, "
                         "loss only on the tokens after `->` (goal, urgency, digit); the checkpoint kept "
                         "is then the best goal NLL")
    ap.add_argument("--device", default=None, help="cpu|mps|cuda (default: auto)")
    args = ap.parse_args()

    device = args.device or (
        "mps" if torch.backends.mps.is_available()
        else "cuda" if torch.cuda.is_available() else "cpu")
    rng = random.Random(args.seed)
    torch.manual_seed(args.seed)

    train_s, val_s, train_docs, val_docs = load_streams(args.data, args.val_frac, rng)
    n_docs = len(train_docs) + len(val_docs)
    print(f"{n_docs} docs -> {len(train_s)} train / {len(val_s)} val tokens, device={device}, loss {args.loss}")
    val_dt = decision_tensors(val_docs, args.max_seq_len)
    goal_ids = sorted({int(t) for t in val_dt[1][torch.arange(len(val_docs)), val_dt[2]]}
                      | {int(d[d.index(tok.encode("->")[0]) + 1]) for d in train_docs})
    train_dt = decision_tensors(train_docs, args.max_seq_len) if args.loss == "decision" else None

    model_args = dict(
        dim=args.dim, n_layers=args.n_layers, n_heads=args.n_heads,
        n_kv_heads=args.n_heads, vocab_size=tok.VOCAB_SIZE,
        multiple_of=32, max_seq_len=args.max_seq_len, dropout=0.05)
    model = Transformer(ModelArgs(**model_args)).to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"model: {n_params/1e6:.2f}M params")

    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01,
                            betas=(0.9, 0.95))
    best_val = best_goal = float("inf")
    best = {}
    t0 = time.time()
    for it in range(1, args.iters + 1):
        frac = it / args.iters
        lr = args.lr * (it / args.warmup if it < args.warmup
                        else 0.5 * (1 + math.cos(math.pi * frac)))
        for g in opt.param_groups:
            g["lr"] = lr
        x, y = (get_decision_batch(train_dt, args.batch, device, rng) if train_dt is not None
                else get_batch(train_s, args.batch, args.max_seq_len, device, rng))
        model(x, y)
        loss = model.last_loss
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()

        if it % 50 == 0:
            print(f"iter {it}/{args.iters} loss {loss.item():.4f} "
                  f"({(time.time()-t0)/it:.2f}s/iter)")
        if it % args.eval_every == 0 or it == args.iters:
            vl = eval_loss(model, val_s, args.batch, args.max_seq_len, device, rng)
            g_nll, g_acc, d_loss = goal_metrics(model, val_dt, goal_ids, device)
            better = g_nll < best_goal if args.loss == "decision" else vl < best_val
            best_val, best_goal = min(best_val, vl), min(best_goal, g_nll)
            marker = ""
            if better:
                best = {"iter": it, "val_loss": vl, "goal_nll": g_nll, "goal_acc": g_acc, "decision_loss": d_loss}
                torch.save({"model_args": model_args, "model": model.state_dict(), "loss": args.loss, **best}, args.out)
                marker = f" -> saved {os.path.basename(args.out)}"
            print(f"iter {it} val_loss {vl:.4f} goal_nll {g_nll:.4f} goal_acc {g_acc:.4f} "
                  f"decision_loss {d_loss:.4f}{marker}")

    peak = (f", peak GPU memory {torch.cuda.max_memory_allocated() / 2**20:.0f} MiB"
            if device == "cuda" else "")
    print(f"done: kept iter {best['iter']}: val_loss {best['val_loss']:.4f} goal_nll {best['goal_nll']:.4f} "
          f"goal_acc {best['goal_acc']:.4f} decision_loss {best['decision_loss']:.4f}, "
          f"checkpoint {args.out}, {n_params / 1e6:.2f}M params{peak}")


if __name__ == "__main__":
    main()

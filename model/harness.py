#!/usr/bin/env python3
"""Train declared runs on every card the lab can spare: plan, launch, poll, collect, probe, report.

  python3 harness.py plan l1-focus2              # lab mode, every card's state, job -> card; refuses a bad config
  python3 harness.py run l1-focus2               # train every seed (one job per card), probe, export, report
  python3 harness.py run l1-focus2 --mode opportunistic   # next to the served models (memory guard, teacher exclusion)
  python3 harness.py status l1-focus2            # each job: done / running (and where) / queued / failed
  python3 harness.py report l1-focus2            # rebuild out/harness/<run>/report.md from the collected jobs
  python3 harness.py selftest                    # the planner, margins, selection and validation on made-up data

Config lives in model/harness/: models.toml, datasets.toml, hosts.toml and runs/<run>.toml
(docs/pipeline.md, "Training harness"). Stdlib only on this side; torch runs in the job's container.

A job is one seed: `harness.py job` inside a container on the card's host (or the local venv for
the local CPU) trains, probes every panel and exports, then writes exit_code. The scheduler
counts a job finished only when that file is there: a failed ssh is "unknown", never "done". A
container that is gone without it crashed; the job is retried once. Everything a job needs is in
its directory, and the local state.json is written before launch, so rerunning `run` after the
harness (or its ssh) died adopts running containers and collects finished ones: no duplicates,
nothing lost. A job is done once out/harness/<run>/<job>/manifest.json exists.
"""
import argparse
import fcntl
import glob
import hashlib
import json
import os
import math
import re
import shlex
import statistics
import subprocess
import sys
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
CONF = os.path.join(HERE, "harness")
OUT = os.path.join(HERE, "out", "harness")
POLL_S = 20
MAX_ATTEMPTS = 2  # the first try and one retry
SCHEMAS = ("2", "3", "4", "L1")
TIE_FLOOR = 0.005  # smallest margins closer than this to the widest are a tie whatever the probe noise


class HarnessError(Exception):
    pass


class HostDown(Exception):
    """no answer from a host: never read as a job's end"""


def say(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ---------------------------------------------------------------- config

def load_toml(path):
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        raise HarnessError(f"missing config: {os.path.relpath(path, HERE)}")
    except tomllib.TOMLDecodeError as e:
        raise HarnessError(f"{os.path.relpath(path, HERE)}: {e}")


def load_config(run_name, conf=CONF):
    hosts = load_toml(os.path.join(conf, "hosts.toml"))
    cfg = {"models": load_toml(os.path.join(conf, "models.toml")),
           "datasets": load_toml(os.path.join(conf, "datasets.toml")),
           "settings": hosts.get("settings", {}), "hosts": hosts.get("hosts", {}),
           "cards": hosts.get("cards", {}), "images": hosts.get("images", {})}
    if run_name is not None:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", run_name):
            raise HarnessError(f"run name {run_name!r}: letters, digits, . _ - only")
        cfg["run"] = load_toml(os.path.join(conf, "runs", run_name + ".toml"))
        cfg["run"]["name"] = run_name
    validate_config(cfg)
    return cfg


def validate_config(cfg):
    """every reference resolves and every value has the type the scheduler expects"""
    errs = []
    for name, m in cfg["models"].items():
        for k in ("dim", "n_layers", "n_heads", "max_seq_len", "mem_mib"):
            if not isinstance(m.get(k), int) or m[k] <= 0:
                errs.append(f"model {name}: {k} must be a positive integer")
        if str(m.get("schema")) not in SCHEMAS:
            errs.append(f"model {name}: schema {m.get('schema')!r} is not one of {', '.join(SCHEMAS)}")
    for name, d in cfg["datasets"].items():
        if not d.get("traces") or not all(isinstance(t, str) for t in d["traces"]):
            errs.append(f"dataset {name}: traces must be a list of globs")
        if str(d.get("schema")) not in SCHEMAS:
            errs.append(f"dataset {name}: schema {d.get('schema')!r} is not one of {', '.join(SCHEMAS)}")
    for name, h in cfg["hosts"].items():
        if "root" not in h:
            errs.append(f"host {name}: root missing")
        if not h.get("ssh") and not h.get("python"):
            errs.append(f"host {name}: a local host needs python (its venv)")
        lk = h.get("lab_key")
        if lk is not None and (lk.get("host") not in cfg["hosts"] or not lk.get("file") or not lk.get("var")):
            errs.append(f"host {name}: lab_key needs host (a known host), file and var")
    for name, c in cfg["cards"].items():
        h = cfg["hosts"].get(c.get("host"))
        if h is None:
            errs.append(f"card {name}: unknown host {c.get('host')!r}")
            continue
        arch = c.get("arch")
        if arch == "cpu":
            if h.get("ssh") and c.get("image") not in cfg["images"]:
                errs.append(f"card {name}: a remote CPU card needs image = one of [images]")
        else:
            if arch not in cfg["images"]:
                errs.append(f"card {name}: arch {arch!r} has no [images.{arch}]")
            if not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", str(c.get("pci", ""))):
                errs.append(f"card {name}: pci must look like 0000:0b:00.0")
            if c.get("mem") not in ("vram", "gtt"):
                errs.append(f"card {name}: mem must be vram or gtt")
            if not h.get("ssh"):
                errs.append(f"card {name}: GPU cards need a remote (docker) host")
    for arch, im in cfg["images"].items():
        if not im.get("tag") or not os.path.isfile(os.path.join(HERE, im.get("dockerfile", ""))):
            errs.append(f"image {arch}: needs tag and an existing dockerfile")
    run = cfg.get("run")
    if run:
        n = run["name"]
        if run.get("model") not in cfg["models"]:
            errs.append(f"run {n}: unknown model {run.get('model')!r} (models.toml has {', '.join(cfg['models'])})")
        if run.get("dataset") not in cfg["datasets"]:
            errs.append(f"run {n}: unknown dataset {run.get('dataset')!r} (datasets.toml has {', '.join(cfg['datasets'])})")
        elif run.get("model") in cfg["models"] and \
                str(cfg["models"][run["model"]]["schema"]) != str(cfg["datasets"][run["dataset"]]["schema"]):
            errs.append(f"run {n}: model and dataset schemas differ")
        seeds = run.get("seeds")
        if not seeds or not all(isinstance(s, int) for s in seeds) or len(set(seeds)) != len(seeds):
            errs.append(f"run {n}: seeds must be a non-empty list of distinct integers")
        for k in ("iters", "batch"):
            if not isinstance(run.get(k), int) or run[k] <= 0:
                errs.append(f"run {n}: {k} must be a positive integer")
        if not isinstance(run.get("lr"), float):
            errs.append(f"run {n}: lr must be a float")
        if run.get("mode", "training") not in ("training", "opportunistic"):
            errs.append(f"run {n}: mode must be training or opportunistic")
        if not isinstance(run.get("tie_floor", TIE_FLOOR), float) or run.get("tie_floor", TIE_FLOOR) < 0:
            errs.append(f"run {n}: tie_floor must be a float >= 0")
        for p in run.get("probes", []):
            if not isinstance(p.get("n"), int) or not isinstance(p.get("seed"), int):
                errs.append(f"run {n}: each [[probes]] needs integer n and seed")
        v = run.get("validate")
        if v:
            panels = len(run.get("probes", []))
            for row, by_seed in v.get("rows", {}).items():
                for s, vals in by_seed.items():
                    if not s.isdigit() or int(s) not in (seeds or []) or len(vals) != panels:
                        errs.append(f"run {n}: validate.rows.{row}.{s}: a run seed with one value per probe panel")
    if errs:
        raise HarnessError("invalid config:\n  " + "\n  ".join(errs))


def job_id(run, seed):
    return f"s{seed}"


# ---------------------------------------------------------------- hosts

class Host:
    def __init__(self, name, conf):
        self.name, self.conf = name, conf
        self.ssh = conf.get("ssh") or None
        self.rel = conf["root"]

    @property
    def local(self):
        return self.ssh is None

    def path(self, *parts):
        """a shell word for a path under the host's root"""
        if self.local:
            return shlex.quote(os.path.join(HERE, self.rel, *parts))
        return '"$HOME"/' + shlex.quote("/".join((self.rel,) + parts))

    def abspath(self, *parts):
        return os.path.join(HERE, self.rel, *parts)

    def sh(self, cmd, timeout=120, stdin=None, check=False):
        argv = (["bash", "-c", cmd] if self.local else
                ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15",
                 "-o", "ServerAliveCountMax=2", self.ssh, cmd])
        try:
            p = subprocess.run(argv, input=stdin, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise HostDown(f"{self.name}: no answer in {timeout} s")
        if not self.local and p.returncode == 255:
            raise HostDown(f"{self.name}: ssh: {p.stderr.strip()[-200:]}")
        if check and p.returncode:
            raise HarnessError(f"{self.name}: `{cmd[:100]}` exit {p.returncode}: {p.stderr.strip()[-600:]}")
        return p

    def rsync(self, src, dst, extra=(), timeout=600):
        try:
            p = subprocess.run(["rsync", "-a", *extra, src, dst], capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise HostDown(f"{self.name}: rsync timed out")
        if p.returncode:
            raise HostDown(f"{self.name}: rsync {src} -> {dst}: exit {p.returncode}: {p.stderr.strip()[-300:]}")

    def put(self, src, rel, extra=()):
        self.rsync(src, self.abspath(rel) if self.local else f"{self.ssh}:{self.rel}/{rel}", extra)

    def get(self, rel, dst):
        self.rsync(self.abspath(rel) if self.local else f"{self.ssh}:{self.rel}/{rel}", dst)


def hosts_of(cfg):
    return {n: Host(n, h) for n, h in cfg["hosts"].items()}


def lab_status(cfg, hosts, name):
    """the llama-swap profile and loaded models on a host: mode training / served / unknown"""
    h = hosts[name]
    lk = h.conf.get("lab_key")
    if not lk:
        return {"mode": "unknown", "why": "no lab_key in hosts.toml"}
    try:
        f = lk["file"].replace("~/", "$HOME/", 1) if lk["file"].startswith("~/") else shlex.quote(lk["file"])
        p = hosts[lk["host"]].sh(f"grep -m1 '^{lk['var']}=' {f}", timeout=30)
        key = p.stdout.strip().split("=", 1)[-1].strip().strip("'\"")
        if not key:
            return {"mode": "unknown", "why": f"{lk['var']} not found on {lk['host']}"}
        # the key travels on stdin and through builtins only: never in an argv
        p = h.sh('read -r H; for u in api/profiles running; do printf "%s\\n" "$H" | '
                 'curl -s -m 8 -H @- "http://127.0.0.1:8080/$u"; echo; done',
                 stdin=f"Authorization: Bearer {key}\n", timeout=40)
        prof, running = (json.loads(x) for x in [ln for ln in p.stdout.splitlines() if ln.strip()][:2])
    except (HostDown, ValueError, KeyError) as e:
        return {"mode": "unknown", "why": str(e)[:200]}
    active = prof.get("active") or None
    loaded = sorted(m["model"] for m in running.get("running", []) if m.get("state") in ("ready", "starting"))
    mode = "training" if active == cfg["settings"].get("training_profile", "training") else "served"
    return {"mode": mode, "profile": active, "loaded": loaded}


def card_state(host, card):
    """free/total MiB as the guard counts them, the render node, and harness containers on the card"""
    pci = card.get("pci")
    cmd = "grep -m1 MemAvailable /proc/meminfo"
    if pci:
        d = f"/sys/bus/pci/devices/{pci}"
        cmd += (f"; cat {d}/mem_info_vram_used {d}/mem_info_vram_total {d}/mem_info_gtt_used {d}/mem_info_gtt_total"
                f"; readlink -f /dev/dri/by-path/pci-{pci}-render")
    if not host.local:
        cmd += f"; echo @@; docker ps --filter label=pt-harness.card={card['name']} --format '{{{{.Names}}}}'"
    out, _, busy = host.sh(cmd, timeout=30, check=True).stdout.partition("@@")
    return parse_card_state(card, out, busy)


def parse_card_state(card, out, busy=""):
    lines = out.split()
    st = {"avail_mib": int(lines[1]) // 1024, "busy": busy.split()}
    if card.get("pci"):
        vu, vt, gu, gt_ = (int(x) >> 20 for x in lines[3:7])
        st.update(render=lines[7], vram_used=vu, vram_total=vt, gtt_used=gu, gtt_total=gt_)
        if card["mem"] == "vram":
            st.update(free=vt - vu, total=vt, used=vu)
        else:
            st.update(free=min(gt_ - gu, st["avail_mib"]), total=gt_, used=gu)
    else:
        st.update(free=st["avail_mib"], total=None, used=None)
    return st


# ---------------------------------------------------------------- planning

def card_verdict(cfg, card, st, lab, mode):
    """(ok, reason) for one card now; raises on a card that can never hold the model"""
    need = cfg["models"][cfg["run"]["model"]]["mem_mib"] + card.get("reserve_mib", 0)
    if st.get("down"):
        return False, f"host down: {st['down']}"
    if st["total"] is not None and st["total"] < need:
        raise HarnessError(f"card {card['name']}: {st['total']} MiB in all, one job needs {need}: remove it from hosts.toml")
    gpu = card["arch"] != "cpu"
    if gpu and mode == "training" and lab.get("mode") != "training":
        return False, f"lab on {card['host']} is {lab.get('mode')} ({lab.get('why') or 'profile ' + str(lab.get('profile'))}): a training-mode run needs the training profile"
    teachers = set(cfg["settings"].get("teacher_models", []))
    if gpu and mode == "opportunistic" and card.get("teacher") and teachers & set(lab.get("loaded", [])):
        return False, "serves the teacher, which is loaded"
    if st["busy"]:
        return False, "busy: " + ", ".join(st["busy"])
    if st["free"] < need:
        return False, f"{st['free']} MiB free < {need} needed: waits"
    return True, f"{st['free']} MiB free >= {need}"


def usable_cards(cfg, allow_cpu):
    return [dict(c, name=n) for n, c in cfg["cards"].items() if c["arch"] != "cpu" or allow_cpu]


def assign(jobs, ok_cards):
    """jobs spread one per card in card order; later jobs queue behind in waves"""
    return [(j, ok_cards[i % len(ok_cards)], i // len(ok_cards) + 1) for i, j in enumerate(jobs)]


def survey(cfg, hosts, mode, allow_cpu, quiet=False):
    """the lab and every card now: {host: lab}, [(card, state, ok, reason)]"""
    cards = usable_cards(cfg, allow_cpu)
    labs = {h: lab_status(cfg, hosts, h) for h in dict.fromkeys(c["host"] for c in cards)}
    out = []
    for c in cards:
        try:
            st = card_state(hosts[c["host"]], c)
        except (HostDown, HarnessError) as e:
            st = {"down": str(e)[:160], "busy": [], "free": 0, "total": None}
        ok, why = card_verdict(cfg, c, st, labs[c["host"]], mode)
        out.append((c, st, ok, why))
    if not quiet:
        for h, lab in labs.items():
            detail = lab.get("why") or f"profile {lab.get('profile') or 'none'}; loaded: {', '.join(lab.get('loaded', [])) or '-'}"
            say(f"lab {h}: {lab['mode']} ({detail})")
        for c, st, ok, why in out:
            say(f"card {c['name']:14s} {c['arch']:8s} {'OK  ' if ok else 'skip'} {why}")
    return labs, out


def cmd_plan(cfg, args):
    run = cfg["run"]
    mode = args.mode or run.get("mode", "training")
    allow_cpu = args.cpu or run.get("cpu", False)
    hosts = hosts_of(cfg)
    say(f"run {run['name']}: model {run['model']}, dataset {run['dataset']}, seeds {run['seeds']}, "
        f"{run['iters']} iters, mode {mode}{', CPU allowed' if allow_cpu else ''}")
    _, cards = survey(cfg, hosts, mode, allow_cpu)
    ok = [c for c, _, k, _ in cards if k]
    if not ok:
        raise HarnessError("no card qualifies now (reasons above); nothing planned")
    jobs = [job_id(run["name"], s) for s in run["seeds"] if not os.path.exists(manifest_path(run["name"], job_id(run["name"], s)))]
    for j, c, wave in assign(jobs, ok):
        say(f"job {j} -> {c['name']} ({c['host']}, {c['arch']}){'' if wave == 1 else f', wave {wave}'}")
    if not jobs:
        say("every job is done: `report` to rebuild the report")
    return 0


# ---------------------------------------------------------------- data

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def prepare_dataset(cfg, name):
    """fold the dataset's traces once (refolded when an input changes); returns (path, sha256)"""
    ds = cfg["datasets"][name]
    inputs = []
    for pat in ds["traces"]:
        hits = sorted(glob.glob(os.path.join(HERE, pat)))
        if not hits:
            raise HarnessError(f"dataset {name}: {pat} matches nothing")
        inputs += hits
    os.makedirs(os.path.join(OUT, "data"), exist_ok=True)
    out, stamp = os.path.join(OUT, "data", name + ".jsonl"), os.path.join(OUT, "data", name + ".json")
    key = {os.path.relpath(p, HERE): sha256(p) for p in inputs}
    try:
        old = json.load(open(stamp))
    except (OSError, ValueError):
        old = {}
    if old.get("inputs") != key or not os.path.exists(out) or sha256(out) != old.get("sha256"):
        say(f"dataset {name}: folding {len(inputs)} files")
        tmp = out + ".tmp"
        subprocess.run([sys.executable, "fold_traces.py", "--schema", str(ds["schema"]), *inputs, "--out", tmp],
                       cwd=HERE, check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, "train_tokenizer.py", "--schema", str(ds["schema"]), "--verify", tmp],
                       cwd=HERE, check=True, stdout=subprocess.DEVNULL)
        os.replace(tmp, out)
        old = {"inputs": key, "sha256": sha256(out), "pairs": sum(1 for _ in open(out))}
        with open(stamp, "w") as f:
            json.dump(old, f, indent=1)
    if ds.get("sha256") and ds["sha256"] != old["sha256"]:
        raise HarnessError(f"dataset {name}: folded to {old['sha256'][:12]}, datasets.toml pins {ds['sha256'][:12]}")
    say(f"dataset {name}: {old['pairs']} pairs, sha256 {old['sha256'][:12]}")
    return out, old["sha256"]


# ---------------------------------------------------------------- jobs (this side)

def run_dir(run):
    return os.path.join(OUT, run)


def manifest_path(run, job):
    return os.path.join(run_dir(run), job, "manifest.json")


def state_path(run, job):
    return os.path.join(run_dir(run), job, "state.json")


def read_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w") as f:
        json.dump(obj, f, indent=1)
    os.replace(path + ".tmp", path)


def container_name(run, job):
    return f"pt-h-{run}-{job}"


def job_spec(cfg, job, seed, data_sha, device):
    run, model = cfg["run"], cfg["models"][cfg["run"]["model"]]
    return {"run": run["name"], "job": job, "seed": seed, "device": device, "schema": str(model["schema"]),
            "data": f"data/{data_sha}.jsonl",
            "train": {"iters": run["iters"], "batch": run["batch"], "lr": run["lr"], "dim": model["dim"],
                      "n_layers": model["n_layers"], "n_heads": model["n_heads"], "max_seq_len": model["max_seq_len"]},
            "probes": run.get("probes", []), "export": run.get("export", True)}


class Prepared:
    """per host: code synced, data uploaded, images present (once per harness process)"""

    def __init__(self):
        self.code, self.data, self.image = set(), set(), {}

    def host(self, cfg, host, run, data_path, data_sha, busy_run):
        if host.name not in self.code:
            if busy_run:
                say(f"{host.name}: a job of this run is still running there; its code is left as is")
            else:
                host.sh(f"mkdir -p {host.path(run, 'code')} {host.path('data')}", check=True)
                host.put(HERE + "/", f"{run}/code/", extra=("--delete", "--copy-links", "--exclude", "out",
                                                            "--exclude", ".venv", "--exclude", "__pycache__",
                                                            "--exclude", "*.pt"))
            self.code.add(host.name)
        if host.name not in self.data:
            if host.sh(f"test -f {host.path('data', data_sha + '.jsonl')}").returncode:
                host.put(data_path, f"data/{data_sha}.jsonl")
            self.data.add(host.name)

    def ensure_image(self, cfg, host, run, arch):
        k = (host.name, arch)
        if k not in self.image:
            im = cfg["images"][arch]
            tag = shlex.quote(im["tag"])
            if host.sh(f"docker image inspect {tag} > /dev/null 2>&1").returncode:
                say(f"{host.name}: building {im['tag']} (first use on this host)")
                code = host.path(run, "code")
                host.sh(f"cd {code} && docker build -q -t {tag} -f {shlex.quote(im['dockerfile'])} docker",
                        timeout=3600, check=True)
            self.image[k] = host.sh(f"docker image inspect -f '{{{{.Id}}}}' {tag}", check=True).stdout.strip()
        return self.image[k]


def launch(cfg, hosts, prep, card, st, seed, data_path, data_sha, running_hosts):
    run = cfg["run"]["name"]
    job = job_id(run, seed)
    host = hosts[card["host"]]
    prep.host(cfg, host, run, data_path, data_sha, host.name in running_hosts)
    image_arch = card["arch"] if card["arch"] != "cpu" else card.get("image")
    image = prep.ensure_image(cfg, host, run, image_arch) if not host.local else None
    gpu = card["arch"] != "cpu"
    spec = job_spec(cfg, job, seed, data_sha, "cuda" if gpu else "cpu")
    old = read_json(state_path(run, job), {})
    state = {"status": "launching", "attempts": old.get("attempts", 0) + 1, "host": host.name, "card": card["name"],
             "arch": card["arch"], "pci": card.get("pci"), "mem": card.get("mem"), "image": image,
             "image_tag": cfg["images"][image_arch]["tag"] if image_arch else None,
             "container": container_name(run, job), "started": time.time(), "spec": spec,
             "mem_used_before": st.get("used"), "mem_used_peak": st.get("used")}
    write_json(state_path(run, job), state)   # before the launch: a rerun then knows what may be running
    jd = host.path(run, job)
    host.sh(f"mkdir -p {jd} && cd {jd} && find . -mindepth 1 -delete && cat > job.json",
            stdin=json.dumps(spec, indent=1), check=True)
    inner = f"python -u harness.py job {{root}} {run}/{job} > {{root}}/{run}/{job}/job.log 2>&1"
    if host.local:
        py = os.path.join(HERE, host.conf["python"])
        root = host.abspath()
        cmd = (f"cd {host.path(run, 'code')} && POCKET_SCHEMA={spec['schema']} setsid -f sh -c "
               + shlex.quote(f"echo $$ > {shlex.quote(root)}/{run}/{job}/pid; exec "
                             + inner.replace("python", shlex.quote(py), 1).format(root=shlex.quote(root))))
    else:
        devices = (f"--device /dev/kfd --device {shlex.quote(st['render'])} "
                   "--group-add $(getent group render | cut -d: -f3) --group-add $(getent group video | cut -d: -f3) "
                   "--security-opt seccomp=unconfined " if gpu else "")
        name = container_name(run, job)
        cmd = (f"docker rm -f {name} > /dev/null 2>&1; docker run -d --name {name} "
               f"--label pt-harness.run={run} --label pt-harness.card={card['name']} {devices}"
               f"-u $(id -u):$(id -g) -e HOME=/tmp -e USER=$USER -e LOGNAME=$USER -e PYTHONUNBUFFERED=1 "
               f"-e POCKET_SCHEMA={spec['schema']} -v {host.path()}:/w -w /w/{run}/code "
               f"{shlex.quote(state['image_tag'])} sh -c {shlex.quote(inner.format(root='/w'))}")
    host.sh(cmd, check=True)
    state["status"] = "launched"
    write_json(state_path(run, job), state)
    say(f"job {job} launched on {card['name']} (attempt {state['attempts']})")
    return state


def poll(hosts, run, job, state):
    """('done', exit code) | ('running', progress line) | ('gone', why); HostDown when no answer"""
    host = hosts[state["host"]]
    jd = host.path(run, job)
    alive = (f"kill -0 $(cat {jd}/pid 2>/dev/null) 2>/dev/null && echo running" if host.local else
             f"docker inspect -f '{{{{.State.Status}}}}' {state['container']} 2>/dev/null")
    mem = (f"cat /sys/bus/pci/devices/{state['pci']}/mem_info_{state['mem']}_used" if state.get("pci") else "true")
    p = host.sh(f"cat {jd}/exit_code 2>/dev/null; echo @@; {alive}; echo @@; "
                f"grep -a '^iter [0-9]*/' {jd}/train.log 2>/dev/null | tail -1; echo @@; {mem}", timeout=40)
    return parse_poll(p.stdout)


def parse_poll(out):
    code, alive, progress, mem = (x.strip() for x in (out.split("@@") + ["", "", "", ""])[:4])
    mem_mib = int(mem) >> 20 if mem.isdigit() else None
    if code.lstrip("-").isdigit():
        return "done", int(code), mem_mib
    if alive in ("running", "created", "restarting"):
        return "running", progress, mem_mib
    return "gone", f"no exit_code and the job is {alive or 'not there'}", mem_mib


def collect(cfg, hosts, run, job, state, data_sha):
    host = hosts[state["host"]]
    dst = os.path.join(run_dir(run), job)
    host.get(f"{run}/{job}/", dst + "/")
    result = read_json(os.path.join(dst, "result.json"), {})
    git = lambda *a: subprocess.run(["git", *a], cwd=HERE, capture_output=True, text=True).stdout.strip()
    manifest = {"run": cfg["run"], "model": cfg["models"][cfg["run"]["model"]], "dataset": cfg["run"]["dataset"],
                "dataset_sha256": data_sha, "seed": state["spec"]["seed"], "host": state["host"], "card": state["card"],
                "arch": state["arch"], "image": state["image"], "image_tag": state["image_tag"],
                "git": {"sha": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain", "--", "."))},
                "attempts": state["attempts"],
                # the job's own time (its stages): the harness may have been away while it ran
                "wall_s": round(sum(s["wall_s"] for s in result.get("stages", [])) or time.time() - state["started"], 1),
                "card_mem_used_before_mib": state.get("mem_used_before"),
                "card_mem_used_peak_mib": state.get("mem_used_peak"), **result}
    write_json(manifest_path(run, job), manifest)
    state["status"] = "collected"
    write_json(state_path(run, job), state)
    if not host.local:
        host.sh(f"docker rm {state['container']} > /dev/null 2>&1")
    t = result.get("train", {})
    say(f"job {job} collected from {state['card']}: val {t.get('best_val')}, {t.get('s_per_iter')} s/iter, "
        f"peak GPU {t.get('peak_gpu_mib')} MiB, {manifest['wall_s']:.0f} s wall")


def cmd_run(cfg, args):
    run = cfg["run"]
    name = run["name"]
    mode = args.mode or run.get("mode", "training")
    allow_cpu = args.cpu or run.get("cpu", False)
    os.makedirs(run_dir(name), exist_ok=True)
    lock = open(os.path.join(run_dir(name), ".lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise HarnessError(f"another harness is running {name}")
    hosts = hosts_of(cfg)
    data_path, data_sha = prepare_dataset(cfg, run["dataset"])
    seeds = {job_id(name, s): s for s in run["seeds"]}
    todo = [j for j in seeds if not os.path.exists(manifest_path(name, j))]
    running, failed = {}, {}
    for j in list(todo):   # adopt what an earlier harness launched
        st = read_json(state_path(name, j))
        if st and st.get("status") in ("launching", "launched"):
            running[j] = st
            todo.remove(j)
            say(f"job {j}: adopting attempt {st['attempts']} on {st['card']}")
        elif st and st.get("status") == "failed":
            failed[j] = st
            todo.remove(j)
    if not todo and not running:
        say("every job is done or failed" + (f" (failed: {', '.join(failed)})" if failed else ""))
        write_report(cfg)
        return 1 if failed else 0
    meta = read_json(os.path.join(run_dir(name), "run.json"), {})
    labs, cards = survey(cfg, hosts, mode, allow_cpu)
    if not running and not any(ok or "waits" in why or why.startswith("busy") for _, _, ok, why in cards):
        raise HarnessError("no card can take a job, now or after waiting (reasons above)" +
                           ("; switch the lab to the training profile, or use --mode opportunistic"
                            if mode == "training" else ""))
    meta.setdefault("started", time.time())
    meta.setdefault("lab_at_start", labs)
    meta["mode"] = mode
    write_json(os.path.join(run_dir(name), "run.json"), meta)
    prep, last_why, last_progress = Prepared(), {}, {}
    while todo or running:
        for j, st in list(running.items()):
            try:
                kind, info, mem = poll(hosts, name, j, st)
            except HostDown as e:
                say(f"job {j}: {e} (still counted as running)")
                continue
            if mem is not None and (st.get("mem_used_peak") or 0) < mem:
                st["mem_used_peak"] = mem
                write_json(state_path(name, j), st)
            if kind == "running":
                if info and info != last_progress.get(j):
                    say(f"job {j} [{st['card']}] {info}")
                    last_progress[j] = info
                continue
            if kind == "done" and info == 0:
                try:
                    collect(cfg, hosts, name, j, st, data_sha)
                    del running[j]
                except HostDown as e:
                    say(f"job {j}: finished, collect failed ({e}); retrying next poll")
                continue
            why = f"exit {info}" if kind == "done" else info
            del running[j]
            if st["attempts"] < MAX_ATTEMPTS:
                say(f"job {j} on {st['card']} crashed ({why}): retrying once")
                todo.insert(0, j)
            else:
                st["status"], st["error"] = "failed", why
                write_json(state_path(name, j), st)
                failed[j] = st
                say(f"job {j} failed twice ({why}); see out/harness/{name}/{j}/ after `report`")
                try:
                    hosts[st["host"]].get(f"{name}/{j}/", os.path.join(run_dir(name), j) + "/")
                except HostDown:
                    pass
        busy_cards = {st["card"] for st in running.values()}
        labs_now = None
        for c in usable_cards(cfg, allow_cpu):
            if not todo:
                break
            if c["name"] in busy_cards:
                continue
            if labs_now is None:
                labs_now = {h: lab_status(cfg, hosts, h) for h in dict.fromkeys(x["host"] for x in usable_cards(cfg, allow_cpu))}
            try:
                cs = card_state(hosts[c["host"]], c)
            except (HostDown, HarnessError) as e:
                cs = {"down": str(e)[:160], "busy": [], "free": 0, "total": None}
            ok, why = card_verdict(cfg, c, cs, labs_now[c["host"]], mode)
            if not ok:
                if last_why.get(c["name"]) != why:
                    say(f"card {c['name']}: {why}")
                    last_why[c["name"]] = why
                continue
            last_why.pop(c["name"], None)
            j = todo.pop(0)
            try:
                running[j] = launch(cfg, hosts, prep, c, cs, seeds[j], data_path, data_sha,
                                    {st["host"] for st in running.values()})
                busy_cards.add(c["name"])
            except (HostDown, HarnessError) as e:
                say(f"job {j}: launch on {c['name']} failed: {e}")
                todo.insert(0, j)
        if todo or running:
            time.sleep(args.poll)
    meta = read_json(os.path.join(run_dir(name), "run.json"), {})
    meta["ended"] = time.time()
    meta["lab_at_end"] = {h: lab_status(cfg, hosts, h) for h in meta.get("lab_at_start", {})}
    write_json(os.path.join(run_dir(name), "run.json"), meta)
    write_report(cfg)
    return 1 if failed else 0


def cmd_status(cfg, args):
    name = cfg["run"]["name"]
    hosts = hosts_of(cfg)
    for s in cfg["run"]["seeds"]:
        j = job_id(name, s)
        m, st = read_json(manifest_path(name, j)), read_json(state_path(name, j))
        if m:
            print(f"{j}: done on {m['card']} (val {m.get('train', {}).get('best_val')})")
        elif not st:
            print(f"{j}: queued")
        elif st["status"] == "failed":
            print(f"{j}: failed on {st['card']} ({st.get('error')})")
        else:
            try:
                kind, info, _ = poll(hosts, name, j, st)
            except HostDown as e:
                kind, info = "unknown", str(e)
            print(f"{j}: attempt {st['attempts']} on {st['card']}: {kind} {info if info is not None else ''}")
    return 0


# ---------------------------------------------------------------- the job (inside the container)

ITER_RE = re.compile(r"^iter (\d+)/(\d+) loss ([\d.]+) \(([\d.]+)s/iter\)")
DONE_RE = re.compile(r"^done: best val_loss ([\d.]+).*?(?:peak GPU memory (\d+) MiB)?$")


def parse_train_log(text):
    out = {}
    for line in text.splitlines():
        m = ITER_RE.match(line)
        if m:
            out.update(iters_done=int(m[1]), loss=float(m[3]), s_per_iter=float(m[4]))
        m = DONE_RE.match(line)
        if m:
            out["best_val"] = float(m[1])
            if m[2]:
                out["peak_gpu_mib"] = int(m[2])
        if " docs -> " in line:
            out["docs"] = int(line.split()[0])
    return out


def cmd_job(args):
    jd = os.path.join(args.root, args.rel)
    spec = read_json(os.path.join(jd, "job.json"))
    env = dict(os.environ, POCKET_SCHEMA=spec["schema"], PYTHONUNBUFFERED="1")
    py, stages, rc = sys.executable, [], 0

    def stage(name, argv):
        t0 = time.time()
        with open(os.path.join(jd, name + ".log"), "w") as log:
            code = subprocess.run([py, "-u", *argv], cwd=HERE, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        stages.append({"stage": name, "exit": code, "wall_s": round(time.time() - t0, 1)})
        print(f"{name}: exit {code} in {time.time() - t0:.0f} s", flush=True)
        return code

    ckpt = os.path.join(jd, "ckpt.pt")
    try:
        t = spec["train"]
        rc = stage("train", ["train.py", "--data", os.path.join(args.root, spec["data"]), "--out", ckpt,
                             "--device", spec["device"], "--seed", str(spec["seed"]), "--iters", str(t["iters"]),
                             "--batch", str(t["batch"]), "--lr", str(t["lr"]), "--dim", str(t["dim"]),
                             "--n-layers", str(t["n_layers"]), "--n-heads", str(t["n_heads"]),
                             "--max-seq-len", str(t["max_seq_len"])])
        for p in spec["probes"] if rc == 0 else []:
            rc = stage(f"probe_p{p['seed']}_n{p['n']}",
                       ["probe_l1.py", "--ckpt", ckpt, "--n", str(p["n"]), "--seed", str(p["seed"]),
                        "--device", spec["device"], "--out", os.path.join(jd, f"probe_p{p['seed']}_n{p['n']}.json")])
            if rc:
                break
        if rc == 0 and spec["export"]:
            rc = (stage("export_q4", ["export_q4.py", os.path.join(jd, "model_q4.bin"), "--checkpoint", ckpt]) or
                  stage("export_fp32", ["llama2.c/export.py", os.path.join(jd, "model_fp32.bin"), "--checkpoint", ckpt]))
        ver = subprocess.run([py, "-c", "import json, torch; c = torch.cuda.is_available(); print(json.dumps("
                              "{'torch': torch.__version__, 'hip': torch.version.hip, 'cuda_available': c, "
                              "'device_name': torch.cuda.get_device_name(0) if c else 'cpu', "
                              "'device_count': torch.cuda.device_count()}))"],
                             capture_output=True, text=True, env=env)
        versions = json.loads(ver.stdout) if ver.returncode == 0 else {"error": ver.stderr[-300:]}
        train = parse_train_log(open(os.path.join(jd, "train.log")).read())
        write_json(os.path.join(jd, "result.json"), {"stages": stages, "train": train, "versions": versions})
    except Exception as e:  # anything else still ends in an exit_code, never a silent hang
        print(f"job error: {e!r}", flush=True)
        rc = rc or 99
    finally:
        with open(os.path.join(jd, "exit_code.tmp"), "w") as f:
            f.write(f"{rc}\n")
        os.replace(os.path.join(jd, "exit_code.tmp"), os.path.join(jd, "exit_code"))
    return rc


# ---------------------------------------------------------------- stats and the report

LIMIT_RE = re.compile(r"^\s*(>=|<=|>|<)\s*([-+]?(?:\d+\.?\d*|\.\d+))")


def margin(value, threshold):
    """distance from the limit on the passing side (negative: failing); None for report-only rows"""
    m = LIMIT_RE.match(threshold or "")
    if not m:
        return None
    lim = float(m[2])
    return value - lim if m[1][0] == ">" else lim - value


def row_key(name):
    """a short unique key for probe_l1's row names"""
    name = name.strip()
    if name.startswith("max single goal"):
        return "content"
    head = name.split()[0]
    if "slope" in name:
        return head + " slope"
    if head == "r8":
        return "r8 " + ("hide" if "P(hide)" in name else "rest")
    if head.startswith("P("):
        return name
    return head


def seed_stats(probes_by_seed):
    """{seed: [probe json per panel]} -> rows {key: {threshold, values {seed: [per panel]}}}, per-seed min margin"""
    rows, per_seed = {}, {}
    for seed, panels in probes_by_seed.items():
        worst = None
        for pi, probe in enumerate(panels):
            for r in probe["rows"]:
                k = row_key(r["row"])
                row = rows.setdefault(k, {"name": r["row"], "threshold": r["threshold"], "values": {}})
                row["values"].setdefault(seed, [None] * len(panels))[pi] = r["value"]
                mg = margin(r["value"], r["threshold"])
                if mg is not None and (worst is None or mg < worst[0]):
                    worst = (mg, k, pi)
        per_seed[seed] = {"min_margin": worst[0] if worst else None, "binding_row": worst[1] if worst else None,
                          "binding_panel": worst[2] if worst else None,
                          "passed": all(p.get("passed") for p in panels)}
    for row in rows.values():
        flat = [v for vs in row["values"].values() for v in vs if v is not None]
        means = [sum(vs) / len(vs) for vs in row["values"].values() if None not in vs]
        mgs = [margin(v, row["threshold"]) for v in flat]
        row["stats"] = {"mean": statistics.fmean(flat), "min": min(flat), "max": max(flat),
                        "sd_all": statistics.stdev(flat) if len(flat) > 1 else 0.0,
                        "sd_between_seeds": statistics.stdev(means) if len(means) > 1 else 0.0,
                        "panel_spread": statistics.fmean(max(vs) - min(vs) for vs in row["values"].values()),
                        "min_margin": min(mgs) if None not in mgs else None}
    return rows, per_seed


def tie_band(per_seed, rows, floor):
    """how far below the best smallest margin a seed still counts as tied: the probe's own noise on
    the rows that bind (their mean panel-to-panel spread), and never less than floor"""
    spreads = [rows[p["binding_row"]]["stats"]["panel_spread"] for p in per_seed.values() if p["binding_row"]]
    return max([floor, *spreads])


def select_seed(per_seed, val_loss, band=0.0):
    """widest smallest margin; seeds within band of it are tied and go to the lower validation loss
    (then the wider margin). None when no seed has a margin"""
    cands = [s for s, v in per_seed.items() if v["min_margin"] is not None]
    if not cands:
        return None
    best = max(per_seed[s]["min_margin"] for s in cands)
    tied = [s for s in cands if per_seed[s]["min_margin"] >= best - band - 1e-9]
    return min(tied, key=lambda s: (val_loss.get(s) if val_loss.get(s) is not None else math.inf,
                                    -per_seed[s]["min_margin"]))


def validate(ref, rows, selected):
    """section-6-style table check: every reference value within tolerance, and the same seed selected"""
    tol, out, ok = ref.get("tolerance", 0.03), [], True
    for k, by_seed in ref.get("rows", {}).items():
        for s, want in by_seed.items():
            got = rows.get(k, {}).get("values", {}).get(int(s))
            for pi, w in enumerate(want):
                g = got[pi] if got and pi < len(got) else None
                dev = None if g is None else g - w
                good = dev is not None and abs(dev) <= tol + 1e-9
                ok &= good
                out.append({"row": k, "seed": int(s), "panel": pi, "want": w, "got": g, "dev": dev, "ok": good})
    sel_ok = "selected" not in ref or ref["selected"] == selected
    return {"tolerance": tol, "checks": out, "rows_ok": ok, "selected_ok": sel_ok, "ok": ok and sel_ok}


def fmt(v, nd=3, sign=False):
    return "-" if v is None else (f"{v:+.{nd}f}" if sign else f"{v:.{nd}f}")


def write_report(cfg):
    run = cfg["run"]
    name, probes = run["name"], run.get("probes", [])
    jobs = {}
    for s in run["seeds"]:
        m = read_json(manifest_path(name, job_id(name, s)))
        if m:
            jobs[s] = m
    panels_by_seed = {}
    for s in jobs:
        jd = os.path.join(run_dir(name), job_id(name, s))
        ps = [read_json(os.path.join(jd, f"probe_p{p['seed']}_n{p['n']}.json")) for p in probes]
        if ps and None not in ps:
            panels_by_seed[s] = ps
    meta = read_json(os.path.join(run_dir(name), "run.json"), {})
    val = {s: m.get("train", {}).get("best_val") for s, m in jobs.items()}
    rows, per_seed = seed_stats(panels_by_seed) if panels_by_seed else ({}, {})
    band = tie_band(per_seed, rows, run.get("tie_floor", TIE_FLOOR)) if per_seed else 0.0
    chosen = select_seed(per_seed, val, band)
    L = [f"# Run {name}", "",
         f"model {run['model']}, dataset {run['dataset']} (sha256 {next(iter(jobs.values()))['dataset_sha256'][:12] if jobs else '-'}), "
         f"{run['iters']} iters, batch {run['batch']}, lr {run['lr']}, mode {meta.get('mode', '-')}; "
         f"{len(jobs)}/{len(run['seeds'])} seeds collected.", ""]
    walls = [m["wall_s"] for m in jobs.values()]
    if meta.get("started") and meta.get("ended"):
        wall = meta["ended"] - meta["started"]
        line = f"Wall time {wall / 60:.1f} min for {len(jobs)} jobs (their sum {sum(walls) / 60:.1f} min"
        if walls:
            line += f", {sum(walls) / wall:.2f}x parallel"
        line += ")"
        base = run.get("validate", {}).get("baseline_wall_s")
        if base:
            line += f"; the baseline was {base / 60:.0f} min ({base / wall:.2f}x)"
        L += [line + ".", ""]
        trained = all(l.get("mode") == "training" for l in meta.get("lab_at_start", {}).values()) and \
            all(l.get("mode") == "training" for l in meta.get("lab_at_end", {}).values())
        seen = ", ".join(f"{h} {lab.get('mode')}" for h, lab in meta.get("lab_at_start", {}).items())
        L += [f"Lab in training mode for the whole run: {f'yes, {wall / 60:.1f} min' if trained else 'no'} "
              f"(at start: {seen}).", ""]
    L += ["## Jobs", "", "| seed | card | host | device | s/iter | train / job wall | peak GPU (torch) | "
          "card mem peak (sysfs) | best val | attempts |", "|---|---|---|---|---|---|---|---|---|---|"]
    for s, m in jobs.items():
        t, v = m.get("train", {}), m.get("versions", {})
        tw = next((x["wall_s"] for x in m.get("stages", []) if x["stage"] == "train"), None)
        L.append(f"| {s} | {m['card']} | {m['host']} | {v.get('device_name', '-')} | {fmt(t.get('s_per_iter'), 2)} | "
                 f"{fmt(tw and tw / 60, 1)} / {m['wall_s'] / 60:.1f} min | {t.get('peak_gpu_mib', '-')} MiB | "
                 f"{m.get('card_mem_used_peak_mib', '-')} MiB (before {m.get('card_mem_used_before_mib', '-')}) | "
                 f"{fmt(t.get('best_val'), 4)} | {m['attempts']} |")
    if jobs:
        v = next(iter(jobs.values())).get("versions", {})
        L += ["", f"torch {v.get('torch')} (HIP {v.get('hip')}); git {next(iter(jobs.values()))['git']}", ""]
    if rows:
        seeds = list(panels_by_seed)
        pnames = [f"p{p['seed']}" for p in probes]
        L += ["## Rows by seed and panel", "", "| row (limit) | " + " | ".join(f"s{s} {' / '.join(pnames)}" for s in seeds)
              + " | mean | sd seeds | panel spread | min margin |", "|---|" + "---|" * (len(seeds) + 4)]
        for k, r in rows.items():
            st = r["stats"]
            cells = [" / ".join(fmt(x) for x in r["values"].get(s, [])) for s in seeds]
            L.append(f"| {k} ({r['threshold']}) | " + " | ".join(cells) +
                     f" | {fmt(st['mean'])} | {fmt(st['sd_between_seeds'])} | {fmt(st['panel_spread'])} | "
                     f"{fmt(st['min_margin'], sign=True)} |")
        L.append("| **smallest margin** | " + " | ".join(
            f"**{fmt(per_seed[s]['min_margin'], sign=True)}** ({per_seed[s]['binding_row']}, {pnames[per_seed[s]['binding_panel']]})"
            for s in seeds) + " | | | | |")
        L += ["", "sd seeds: the standard deviation of the per-seed means (training-seed variance); panel spread: the mean "
              "max - min across one seed's panels (probe sampling noise). A row whose sd seeds is near its min margin "
              "can flip on another seed.", ""]
        ranked = sorted(seeds, key=lambda s: per_seed[s]["min_margin"] if per_seed[s]["min_margin"] is not None else -9, reverse=True)
        best = per_seed[ranked[0]]["min_margin"]
        L += ["## Selection", "", f"Rule: the widest smallest margin over every limited row and panel. Seeds within "
              f"{band:.3f} of the widest are tied (the binding rows' panel spread, at least {run.get('tie_floor', TIE_FLOOR)}) "
              "and go to the lower val loss.", ""]
        for i, s in enumerate(ranked, 1):
            p = per_seed[s]
            tied = best is not None and p["min_margin"] is not None and p["min_margin"] >= best - band - 1e-9
            L.append(f"{i}. seed {s}: smallest margin {fmt(p['min_margin'], sign=True)} on {p['binding_row']}, "
                     f"val {fmt(val.get(s), 4)}, {'all rows pass' if p['passed'] else 'a row FAILS'}"
                     + (", tied for best" if tied and len(seeds) > 1 else "") + ("  <- selected" if s == chosen else ""))
        sel = jobs.get(chosen)
        if sel:
            L += ["", f"Selected: seed {chosen}, `out/harness/{name}/{job_id(name, chosen)}/model_q4.bin`" +
                  ("" if per_seed[chosen]["passed"] else " (it still FAILS a row: do not ship)"), ""]
    ref = run.get("validate")
    vres = None
    if ref and rows:
        vres = validate(ref, rows, chosen)
        L += ["## Validation against the reference", "",
              f"Tolerance ±{vres['tolerance']}: {'every row within' if vres['rows_ok'] else 'rows OUTSIDE'}; "
              f"selected seed {chosen} {'matches' if vres['selected_ok'] else 'differs from'} the reference "
              f"({ref.get('selected')}). **{'PASS' if vres['ok'] else 'FAIL'}**", "",
              "| row | seed | panel | reference | now | dev |", "|---|---|---|---|---|---|"]
        for c in vres["checks"]:
            L.append(f"| {c['row']} | {c['seed']} | p{probes[c['panel']]['seed']} | {fmt(c['want'])} | {fmt(c['got'])} | "
                     f"{fmt(c['dev'], sign=True)}{'' if c['ok'] else ' **out**'} |")
    os.makedirs(run_dir(name), exist_ok=True)
    with open(os.path.join(run_dir(name), "report.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    write_json(os.path.join(run_dir(name), "report.json"),
               {"run": name, "jobs": jobs, "rows": {k: {**r, "values": {str(s): v for s, v in r["values"].items()}}
                                                     for k, r in rows.items()},
                "per_seed": {str(s): v for s, v in per_seed.items()}, "selected": chosen, "validation": vres})
    say(f"report: out/harness/{name}/report.md" + (f"; selected seed {chosen}" if chosen is not None else ""))
    return chosen


def cmd_report(cfg, args):
    write_report(cfg)
    print(open(os.path.join(run_dir(cfg["run"]["name"]), "report.md")).read())
    return 0


# ---------------------------------------------------------------- selftest

def selftest():
    import tempfile
    # the planner's config checks: unknown model / dataset, a card that can never fit
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "runs"))
        for f in ("models.toml", "datasets.toml", "hosts.toml"):
            with open(os.path.join(CONF, f), "rb") as src, open(os.path.join(tmp, f), "wb") as dst:
                dst.write(src.read())
        good = open(os.path.join(CONF, "runs", "l1-focus2.toml")).read()
        for bad, want in ((good.replace('model = "l1-14m"', 'model = "l1-99m"'), "unknown model 'l1-99m'"),
                          (good.replace('dataset = "l1-focus2"', 'dataset = "nope"'), "unknown dataset 'nope'"),
                          (good.replace("seeds = [1337, 1, 2]", "seeds = [1, 1]"), "distinct"),
                          (good.replace("\n[[probes]]\nn = 400\nseed = 23\n", "\n"), "one value per probe panel")):
            with open(os.path.join(tmp, "runs", "x.toml"), "w") as f:
                f.write(bad)
            try:
                load_config("x", tmp)
                raise AssertionError(f"accepted a config with {want}")
            except HarnessError as e:
                assert want in str(e), (want, str(e))
        with open(os.path.join(tmp, "runs", "x.toml"), "w") as f:
            f.write(good)
        cfg = load_config("x", tmp)
    # the guard: memory, lab mode, teacher, a card too small for the model at all
    card = dict(cfg["cards"]["zaphod-card2"], name="zaphod-card2")
    gib = 1 << 30
    sysfs = "MemAvailable: 9000000 kB\n{} {} 0 0\n/dev/dri/renderD129\n"
    full = parse_card_state(card, sysfs.format(30 * gib, 32 * gib))
    roomy = parse_card_state(card, sysfs.format(4 * gib, 32 * gib))
    assert (full["free"], roomy["free"], roomy["render"]) == (2048, 28672, "/dev/dri/renderD129")
    served, training = {"mode": "served", "profile": None, "loaded": ["chat-teacher"]}, {"mode": "training", "loaded": []}
    assert card_verdict(cfg, card, roomy, training, "training")[0]
    ok, why = card_verdict(cfg, card, full, training, "training")
    assert not ok and "waits" in why, why
    assert not card_verdict(cfg, card, roomy, served, "training")[0]
    ok, why = card_verdict(cfg, card, roomy, served, "opportunistic")
    assert not ok and "teacher" in why, why
    assert card_verdict(cfg, card, roomy, dict(served, loaded=["coder"]), "opportunistic")[0]
    assert not card_verdict(cfg, card, dict(roomy, busy=["pt-h-x-s1"]), training, "training")[0]
    try:
        card_verdict(cfg, card, parse_card_state(card, sysfs.format(0, 2 * gib)), training, "training")
        raise AssertionError("a 2 GiB card passed for a job needing 3 GiB")
    except HarnessError as e:
        assert "in all" in str(e)
    gtt = dict(cfg["cards"]["marvin-8060s"], name="marvin-8060s")   # unified memory: the smaller of GTT and RAM
    st = parse_card_state(gtt, f"MemAvailable: {20 * 1024 * 1024} kB\n{300 << 20} {512 << 20} {78 * gib} {120 * gib}\n/dev/dri/renderD128\n")
    assert st["free"] == 20 * 1024, st
    assert [(j, c, w) for j, c, w in assign(["a", "b", "c", "d"], ["x", "y", "z"])] == \
        [("a", "x", 1), ("b", "y", 1), ("c", "z", 1), ("d", "x", 2)]
    # polling: only an exit_code ends a job; a vanished container without one is a crash
    assert parse_poll("0\n@@\nexited\n@@\n@@\n1073741824\n")[:2] == ("done", 0)
    assert parse_poll("@@\nrunning\n@@\niter 50/5000 loss 1.2 (0.15s/iter)\n@@\n")[:2] == ("running", "iter 50/5000 loss 1.2 (0.15s/iter)")
    assert parse_poll("@@\n@@\n@@\n")[0] == "gone" and parse_poll("@@\nexited\n@@\n@@\n")[0] == "gone"
    t = parse_train_log("50968 docs -> 1 train / 1 val tokens, device=cuda\niter 50/100 loss 2.0 (0.20s/iter)\n"
                        "iter 100/100 loss 1.5 (0.16s/iter)\ndone: best val_loss 0.7445, checkpoint x, peak GPU memory 812 MiB")
    assert t == {"docs": 50968, "iters_done": 100, "loss": 1.5, "s_per_iter": 0.16, "best_val": 0.7445, "peak_gpu_mib": 812}, t
    assert parse_train_log("done: best val_loss 0.9, checkpoint x")["best_val"] == 0.9
    # margins and selection, on rows as probe_l1 writes them
    for v, th, want in ((0.918, ">= 0.86", 0.058), (0.001, "<= 0.05", 0.049), (0.402, "< 0.45", 0.048),
                        (0.07, "> 0", 0.07), (0.975, ">= 0.10 (now and then)", 0.875), (0.3, "-", None), (0.3, "", None)):
        got = margin(v, th)
        assert (got is None and want is None) or abs(got - want) < 1e-9, (th, got)

    def probe(a2, a12, content, passed=True):
        return {"passed": passed, "rows": [
            {"row": "A2  P(hunt | ...)", "value": a2, "threshold": ">= 0.86"},
            {"row": "A11 P(explore+investigate) bold 9 - bold 0 (paired)", "value": 0.5, "threshold": ">= 0.15"},
            {"row": "A12 P(investigate | social 0, other near)", "value": a12, "threshold": "<= 0.05"},
            {"row": "P(last kept | active, bored 0-1, any last) (report)", "value": 0.1, "threshold": "-"},
            {"row": "max single goal | content (explore)", "value": content, "threshold": "< 0.45"},
            {"row": "A11 slope of P(explore+investigate), bold 0..9", "value": 0.06, "threshold": "> 0"}]}
    rows, per = seed_stats({1337: [probe(0.966, 0.0, 0.423), probe(0.966, 0.0, 0.395)],
                            1: [probe(0.949, 0.0, 0.411), probe(0.954, 0.0, 0.406)],
                            2: [probe(0.918, 0.001, 0.402), probe(0.932, 0.001, 0.381)]})
    assert set(rows) == {"A2", "A11", "A12", "content", "A11 slope", "P(last kept | active, bored 0-1, any last) (report)"}
    assert [round(per[s]["min_margin"], 3) for s in (1337, 1, 2)] == [0.027, 0.039, 0.048], per
    assert per[2]["binding_row"] == "content" and per[2]["binding_panel"] == 0
    band = tie_band(per, rows, TIE_FLOOR)   # content binds every seed; its panel spreads 0.028 / 0.005 / 0.021
    assert abs(band - statistics.fmean([0.028, 0.005, 0.021])) < 1e-9, band
    assert select_seed(per, {}) == 2                                   # no band: the widest alone
    assert select_seed(per, {1337: 0.70, 1: 0.7506, 2: 0.7445}, band) == 2   # s1 tied (0.009 below), s1337 not (0.021)
    assert select_seed(per, {1337: 0.70, 1: 0.7400, 2: 0.7445}, band) == 1   # inside the band the val loss decides
    assert select_seed(per, {}, band) == 2                             # no val losses: the wider margin
    assert select_seed(per, {1337: 0.70}, 0.05) == 1337                # a band wide enough ties all three
    tie = {1: {"min_margin": 0.04}, 2: {"min_margin": 0.04}}
    assert select_seed(tie, {1: 0.75, 2: 0.74}) == 2 and select_seed(tie, {1: 0.73, 2: 0.74}) == 1
    assert abs(rows["A2"]["stats"]["sd_between_seeds"] - statistics.stdev([0.966, 0.9515, 0.925])) < 1e-9
    assert rows["P(last kept | active, bored 0-1, any last) (report)"]["stats"]["min_margin"] is None
    ref = {"tolerance": 0.03, "selected": 2, "rows": {"A2": {"1337": [0.966, 0.966], "2": [0.918, 0.932]}}}
    assert validate(ref, rows, 2)["ok"]
    assert not validate(ref, rows, 1)["ok"] and validate(ref, rows, 1)["rows_ok"]
    off = {"tolerance": 0.03, "rows": {"A2": {"2": [0.95, 0.932]}}}   # 0.918 vs 0.95: 0.032 out
    assert not validate(off, rows, 2)["rows_ok"]
    assert not validate({"rows": {"A9": {"2": [0.9, 0.9]}}}, rows, 2)["ok"]   # a reference row the probe lacks
    print("selftest OK: config refusals, guard (memory, lab mode, teacher, busy, too small), GTT accounting, "
          "card waves, poll states, train-log parsing, margins, the section-6 smallest margins "
          "(+0.027 / +0.039 / +0.048 -> seed 2), ties, validation")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("plan", "run", "status", "report"):
        p = sub.add_parser(c)
        p.add_argument("run", help="a run in harness/runs/ (without .toml)")
        if c in ("plan", "run"):
            p.add_argument("--mode", choices=("training", "opportunistic"), help="override the run's mode")
            p.add_argument("--cpu", action="store_true", help="allow CPU cards")
        if c == "run":
            p.add_argument("--poll", type=float, default=POLL_S, help="seconds between polls")
    p = sub.add_parser("job", help="(inside the container) train, probe and export one job")
    p.add_argument("root")
    p.add_argument("rel")
    sub.add_parser("selftest")
    args = ap.parse_args()
    if args.cmd == "job":
        return cmd_job(args)
    if args.cmd == "selftest":
        return selftest()
    try:
        cfg = load_config(args.run)
        return {"plan": cmd_plan, "run": cmd_run, "status": cmd_status, "report": cmd_report}[args.cmd](cfg, args)
    except HarnessError as e:
        print(f"harness: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

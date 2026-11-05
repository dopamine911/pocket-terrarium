#!/usr/bin/env python3
"""model_trailer.py - the model partition's validity marker, as a file.

The tank's model partition ends with a 4 KB trailer (firmware/main/
model_trailer.c): magic, version, the body's length and sha256, a tag, a
crc. The cable installer writes it as its own part at the partition's last
sector, and flash.sh writes it beside the model; a tank with none heals it
at boot from the values in common/version.h.

Each world ships its own model (common/world.h THE BRAIN): the aquarium the
fish model, the terrarium the lizard model. The world is the build's
(<build dir>/config/sdkconfig.h), or --world.

  tools/model_trailer.py --build-dir B                # the world's model -> B/model_trailer.bin
  tools/model_trailer.py --build-dir B --check        # also refuse if version.h disagrees with the file
  tools/model_trailer.py --build-dir B --print-model  # the world's model file, for the flash scripts
  tools/model_trailer.py --model X --out Y --tag v3m --world aquarium

--check is what a flash or a release runs: PT_MODEL_TAG_<WORLD>,
PT_MODEL_LEN_<WORLD> and PT_MODEL_SHA256_<WORLD> in common/version.h must
describe the model file that ships, or the tank would heal a wrong trailer
and, later, refuse a good model update. Update version.h when a model
changes."""
import argparse, hashlib, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAGIC = 0x54524D50           # "PMRT"
SIZE = 0x1000
WORLDS = ("aquarium", "terrarium")

def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h

def world_of_build(build_dir):
    """the build's world from its generated sdkconfig.h; a build with none is the Kconfig default, the terrarium"""
    try:
        cfg = open(os.path.join(build_dir, "config", "sdkconfig.h")).read()
    except OSError:
        sys.exit(f"model_trailer: {build_dir} has no config/sdkconfig.h - build it first, or pass --world")
    return "aquarium" if re.search(r"#define CONFIG_POCKET_TANK_WORLD_AQUARIUM 1\b", cfg) else "terrarium"

def model_file(world):
    """the world's model file (common/world.h PT_MODEL_FILE_<WORLD>), an absolute path"""
    src = open(os.path.join(ROOT, "common", "world.h")).read()
    rel = re.search(rf'#define PT_MODEL_FILE_{world.upper()}\s+"([^"]+)"', src).group(1)
    return os.path.join(ROOT, rel)

def version_h(world):
    """the world's model identity in common/version.h: (tag, length, sha256)"""
    src = open(os.path.join(ROOT, "common", "version.h")).read()
    w = world.upper()
    tag = re.search(rf'#define PT_MODEL_TAG_{w}\s+"([^"]+)"', src).group(1)
    ln = int(re.search(rf'#define PT_MODEL_LEN_{w}\s+(\d+)u?', src).group(1))
    sha = re.search(rf'#define PT_MODEL_SHA256_{w}\s+"([0-9a-f]{{64}})"', src).group(1)
    return tag, ln, sha

def build(model_path, tag):
    body = open(model_path, "rb").read()
    sha = hashlib.sha256(body).digest()
    head = struct.pack("<III32s16s", MAGIC, 1, len(body), sha, tag.encode()[:15].ljust(16, b"\0"))
    blob = head + struct.pack("<I", fnv1a(head))
    return blob.ljust(SIZE, b"\xff"), len(body), sha.hex()

def main():
    a = argparse.ArgumentParser()
    a.add_argument("--build-dir", default=os.path.expanduser("~/.cache/pocket-terrarium/fw-build"))
    a.add_argument("--world", choices=WORLDS, default=None, help="default: the build dir's")
    a.add_argument("--model", default=None, help="default: the world's (common/world.h)")
    a.add_argument("--out", default=None, help="default: <build dir>/model_trailer.bin")
    a.add_argument("--tag", default=None, help="default: the world's PT_MODEL_TAG_<WORLD> from common/version.h")
    a.add_argument("--check", action="store_true", help="refuse if common/version.h disagrees with the model file")
    a.add_argument("--print-model", action="store_true", help="print the world's model file and stop")
    a = a.parse_args()
    world = a.world or world_of_build(a.build_dir)
    model = a.model or model_file(world)
    if a.print_model:
        print(model)
        return
    tag, ln, sha = version_h(world)
    if a.tag: tag = a.tag
    blob, got_len, got_sha = build(model, tag)
    if a.check and (got_len != ln or got_sha != sha):
        sys.exit(f"model_trailer: common/version.h says the {world}'s model is {tag} {ln} B {sha[:16]}..., {model} is "
                 f"{got_len} B {got_sha[:16]}... - update PT_MODEL_LEN_{world.upper()} / PT_MODEL_SHA256_{world.upper()}")
    out = a.out or os.path.join(a.build_dir, "model_trailer.bin")
    if out != os.devnull:
        os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(blob)
    print(f"model_trailer: {out} - the {world}'s {tag}, {got_len:,} B, sha256 {got_sha[:16]}..."
          f"{' (matches version.h)' if (got_len, got_sha) == (ln, sha) else ' (version.h DIFFERS)'}")

if __name__ == "__main__":
    main()

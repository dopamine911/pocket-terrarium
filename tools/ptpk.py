#!/usr/bin/env python3
"""tools/ptpk.py - author, read and check a PTPK pack (docs/ptpk.md is the byte-exact contract).

    ptpk.py build SRC.toml [-o F]   compile one TOML source into a pack (a new pack for the card; default F: ./<uid_ns>.ptpk)
    ptpk.py dump FILE.ptpk          check the file and print the pack as a source
    ptpk.py verify FILE.ptpk        check the header, length, CRC and records; exit 1 when the file is bad

The built-in packs are not made here: the C const structs are their source and tools/ptpk_emit.c writes packs/*.ptpk.
Python 3.11+, standard library only. Exit 0 ok, 1 bad file (verify), 2 usage or source error.
"""
import sys

if sys.version_info < (3, 11):
    sys.stderr.write("ptpk.py: needs Python 3.11 or newer (tomllib); this is Python %d.%d\n" % sys.version_info[:2])
    sys.exit(2)

import argparse
import re
import struct
import tomllib
import zlib
from decimal import Decimal
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------------------------------------------- format

FMT = 1
KIND_SPECIES, KIND_HABITAT = 1, 2
KIND_NAMES = {"species": KIND_SPECIES, "habitat": KIND_HABITAT}
WORLDS = {"aquarium": 1, "terrarium": 2}                      # world.h PT_WORLD_*
BODY_MAX = 2048
REQUIRED = 0x8000

# the world's goal names, in bit order (tank.c FISH_GOAL_NAMES / LZ_GOAL_NAMES)
GOAL_NAMES = {
    1: ["seek_food", "flee_shadow", "visit_bubbles", "follow_friend", "explore", "rest", "dart_play", "inspect_reef"],
    2: ["hunt", "bask", "hide", "explore", "investigate", "dig", "rest"],
}

SCHEDULES = {"day": 0, "dusk": 1, "night": 2}
SHED_STYLES = {"whole": 0, "patches": 1}
PLANS = {"lizard": 0, "fish": 1}
HIDE_KINDS = {"warm": 0, "humid": 1, "cool": 2}
DIET_BITS = {"cricket": 1, "mealworm": 2, "greens": 4, "pellet": 8}

# id -> (name, REQUIRED, applies to kind: 0 both / 1 species / 2 habitat)
R_NAME, R_UID_NS, R_WORLD, R_LEGACY, R_TEST = 0x0001, 0x0002, 0x0003, 0x0004, 0x0005
R_HABITATS, R_ACTIVITY, R_HEAT, R_DRIVES, R_WALK, R_LIFE, R_SHED, R_DIET, R_BODY, R_MORPHS, R_PATTERNS, R_GOALS = (
    0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106, 0x0107, 0x0108, 0x0109, 0x010A, 0x010B, 0x010C)
R_CLIMATE, R_BASK, R_HIDES, R_ROCKS, R_RAMPS, R_MOISTURE, R_DISH = 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207
RECORDS = {
    R_NAME: ("NAME", True, 0), R_UID_NS: ("UID_NS", True, 0), R_WORLD: ("WORLD", True, 0),
    R_LEGACY: ("LEGACY_ID", False, 0), R_TEST: ("TEST_ONLY", False, 0),
    R_HABITATS: ("HABITATS", True, 1), R_ACTIVITY: ("ACTIVITY", True, 1), R_HEAT: ("HEAT", True, 1),
    R_DRIVES: ("DRIVES", True, 1), R_WALK: ("WALK", True, 1), R_LIFE: ("LIFE", True, 1), R_SHED: ("SHED", True, 1),
    R_DIET: ("DIET", True, 1), R_BODY: ("BODY", True, 1), R_MORPHS: ("MORPHS", False, 1),
    R_PATTERNS: ("PATTERNS", False, 1), R_GOALS: ("GOALS", True, 1),
    R_CLIMATE: ("CLIMATE", True, 2), R_BASK: ("BASK", True, 2), R_HIDES: ("HIDES", True, 2),
    R_ROCKS: ("ROCKS", True, 2), R_RAMPS: ("RAMPS", True, 2), R_MOISTURE: ("MOISTURE", True, 2),
    R_DISH: ("DISH", True, 2),
}

# fixed-layout records: id -> (source section, [(key, type, arg)]); type f32 (arg = count), enum (arg = map), bool, u8
FIXED = {
    R_ACTIVITY: ("activity", [("schedule", "enum", SCHEDULES), ("settle_s", "f32", 1)]),
    R_HEAT: ("heat", [("t_cold", "f32", 1), ("t_lo", "f32", 1), ("t_hi", "f32", 1), ("t_hot", "f32", 1),
                      ("tau_s", "f32", 4)]),
    R_DRIVES: ("drives", [(k, "f32", 1) for k in (
        "hunger_per_s", "energy_per_s", "energy_rest_per_s", "curiosity_per_s", "hunger_per_cricket",
        "sleep_hunger_per_h", "sleep_energy_per_h", "sleep_stress_per_h")]),
    R_WALK: ("walk", [("walk_px_s", "f32", 4), ("bout_s", "f32", 2), ("pause_s", "f32", 2), ("turn_s", "f32", 1)]),
    R_LIFE: ("life", [("stage_s", "f32", 3), ("pop_cap", "u8", None)]),
    R_SHED: ("shed", [("sheds", "bool", None), ("style", "enum", SHED_STYLES), ("every_s", "f32", 4),
                      ("soon_s", "f32", 1), ("window_s", "f32", 1)]),
    R_BODY: ("body", [("plan", "enum", PLANS), ("length_px", "f32", 1), ("head_w", "f32", 1), ("limb_len", "f32", 1),
                      ("tail_fat", "f32", 1), ("eye_r", "f32", 1), ("eyelids", "bool", None)]),
    R_CLIMATE: ("climate", [("floor_warm_c", "f32", 1), ("floor_cool_c", "f32", 1), ("night_drop_c", "f32", 1),
                            ("rise_c", "f32", 1)]),
    R_BASK: ("bask", [("surface", "u8", None), ("u", "f32", 1), ("c", "f32", 1), ("r_px", "f32", 1)]),
    R_MOISTURE: ("moisture", [("start", "f32", 1), ("dry_per_s", "f32", 1), ("humidity", "f32", 1)]),
    R_DISH: ("dish", [("u", "f32", 1)]),
}
# sections whose absence means "the record is absent" (everything else is a missing REQUIRED section)
OPTIONAL_SECTIONS = {"walk", "shed", "dish", "hides", "rocks", "ramps", "morphs", "patterns"}
SPECIES_SECTIONS = ["pack", "activity", "heat", "drives", "walk", "life", "shed", "diet", "body", "morphs", "patterns", "goals"]
HABITAT_SECTIONS = ["pack", "climate", "bask", "hides", "rocks", "ramps", "moisture", "dish"]
ARRAY_SECTIONS = {"morphs", "hides", "rocks", "ramps"}
SEC_RECORD = {sec: rid for rid, (sec, _f) in FIXED.items()}
SEC_RECORD.update({"diet": R_DIET, "morphs": R_MORPHS, "patterns": R_PATTERNS, "goals": R_GOALS,
                   "hides": R_HIDES, "rocks": R_ROCKS, "ramps": R_RAMPS})

UID_NS_RE = re.compile(r"[a-z0-9._-]{3,47}\Z")


class ToolError(Exception):
    """a usage, source or pack error: one line, exit 2"""


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def rel(path):
    p = Path(path)
    try:
        return str(p.resolve().relative_to(ROOT))
    except (ValueError, OSError):
        return str(p)


# ----------------------------------------------------------------------------------------------- exact binary32 rounding


def f32_bits(v, allow_subnormal=False):
    """int or Decimal -> the nearest binary32's bits, ties to even, exact (no double). ValueError on nan/inf/overflow/underflow."""
    if isinstance(v, Decimal):
        if not v.is_finite():
            raise ValueError("not a finite number")
        sign = 1 if v.is_signed() else 0
        if v == 0:
            return sign << 31
        adj = v.adjusted()
        if adj > 40:
            raise ValueError("too large for a binary32 (max 3.4028235e38)")
        if adj < -60:
            raise ValueError("too small for a binary32 (min normal 1.17549435e-38)")
        x = Fraction(v.copy_abs())
    else:
        sign = 1 if v < 0 else 0
        if v == 0:
            return 0
        if abs(v) >= 1 << 130:
            raise ValueError("too large for a binary32 (max 3.4028235e38)")
        x = Fraction(abs(v))
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()                      # floor(log2 x) is e or e - 1
    lhs, rhs = (n, d << e) if e >= 0 else (n << -e, d)
    if lhs < rhs:
        e -= 1
    qexp = max(e, -126) - 23                                 # the quantum: 2^qexp
    num, den = (n, d << qexp) if qexp >= 0 else (n << -qexp, d)
    q, r = divmod(num, den)
    if 2 * r > den or (2 * r == den and q & 1):
        q += 1
    if q >= 1 << 23:                                         # normal
        if q == 1 << 24:
            q, e = 1 << 23, e + 1
        e = max(e, -126)
        if e > 127:
            raise ValueError("too large for a binary32 (max 3.4028235e38)")
        return (sign << 31) | ((e + 127) << 23) | (q - (1 << 23))
    if not allow_subnormal:
        raise ValueError("too small for a binary32 (min normal 1.17549435e-38)")
    return (sign << 31) | q


def f32_value(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def f32_text(bits):
    """the shortest decimal that converts back to exactly these bits (a TOML float)"""
    v = f32_value(bits)
    for p in range(1, 10):
        s = "%.*g" % (p, v)
        if f32_bits(Decimal(s), True) == bits:
            break
    else:
        raise AssertionError("no decimal round trip for %08x" % bits)
    if "e" in s and 1e-4 <= abs(v) < 1e7:                    # positional reads better: 70.0, not 7e+01
        s = format(Decimal(s), "f")
    return s if ("." in s or "e" in s) else s + ".0"


# ------------------------------------------------------------------------------------------------------- source parsing


def text_problem(s, lo, hi):
    if not isinstance(s, str):
        return "must be a string"
    if not lo <= len(s) <= hi:
        return "must be %d..%d bytes (is %d)" % (lo, hi, len(s))
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in s):
        return "has a byte outside 0x20..0x7e (printable ASCII)"
    if s[0] == " " or s[-1] == " ":
        return "starts or ends with a space"
    return None


class Sec:
    """one TOML table with the key accounting that makes an unknown key an error"""

    def __init__(self, path, name, d):
        self.path, self.name, self.d, self.used = path, name, d, set()
        if not isinstance(d, dict):
            raise ToolError("%s: [%s]: must be a table" % (rel(path), name))

    def err(self, key, msg):
        raise ToolError("%s: [%s] %s: %s" % (rel(self.path), self.name, key, msg))

    def has(self, key):
        return key in self.d

    def raw(self, key):
        if key not in self.d:
            self.err(key, "missing")
        self.used.add(key)
        return self.d[key]

    def finish(self):
        for k in self.d:
            if k not in self.used:
                self.err(k, "unknown key")

    def _bits(self, key, v):
        if isinstance(v, float):
            self.err(key, "nan and inf are not allowed")
        if isinstance(v, bool) or not isinstance(v, (Decimal, int)):
            self.err(key, "must be a number")
        try:
            return f32_bits(v)
        except ValueError as e:
            self.err(key, "%s (%s)" % (e, v))

    def f32(self, key):
        return self._bits(key, self.raw(key))

    def f32list(self, key, n):
        v = self.raw(key)
        if not isinstance(v, list) or len(v) != n:
            self.err(key, "must be a list of %d numbers" % n)
        return [self._bits(key, x) for x in v]

    def uint(self, key, lo, hi):
        v = self.raw(key)
        if isinstance(v, bool) or not isinstance(v, int):
            self.err(key, "must be an integer")
        if not lo <= v <= hi:
            self.err(key, "must be %d..%d (is %d)" % (lo, hi, v))
        return v

    def boolean(self, key):
        v = self.raw(key)
        if not isinstance(v, bool):
            self.err(key, "must be true or false")
        return v

    def text(self, key, lo, hi):
        v = self.raw(key)
        p = text_problem(v, lo, hi)
        if p:
            self.err(key, p)
        return v

    def enum(self, key, m):
        v = self.raw(key)
        if not isinstance(v, str) or v not in m:
            self.err(key, "must be one of %s" % ", ".join('"%s"' % k for k in m))
        return m[v]

    def strlist(self, key, lo, hi):
        v = self.raw(key)
        if not isinstance(v, list) or not lo <= len(v) <= hi or not all(isinstance(x, str) for x in v):
            self.err(key, "must be a list of %d..%d strings" % (lo, hi))
        return v


def u32(v):
    return struct.pack("<I", v)


def enc_fixed(sec, fields):
    out = bytearray()
    for key, typ, arg in fields:
        if typ == "f32":
            for b in ([sec.f32(key)] if arg == 1 else sec.f32list(key, arg)):
                out += u32(b)
        elif typ == "enum":
            out.append(sec.enum(key, arg))
        elif typ == "bool":
            out.append(1 if sec.boolean(key) else 0)
        else:
            out.append(sec.uint(key, 0, 255))
    sec.finish()
    return bytes(out)


def s8(s):
    b = s.encode("ascii")
    return bytes([len(b)]) + b


class Pack:
    def __init__(self, path, kind, uid_ns, name, world, legacy_id, test_only, habitats, blob):
        self.path, self.kind, self.uid_ns, self.name, self.world = path, kind, uid_ns, name, world
        self.legacy_id, self.test_only, self.habitats, self.blob = legacy_id, test_only, habitats, blob
        self.uid = fnv1a32(uid_ns.encode("ascii"))


def compile_source(path):
    """one source file -> Pack. `habitats` stay as written (uid_ns strings or integer uids)."""
    path = Path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        raise ToolError("%s: cannot read: %s" % (rel(path), e))
    try:
        doc = tomllib.loads(text, parse_float=Decimal)
    except tomllib.TOMLDecodeError as e:
        raise ToolError("%s: %s" % (rel(path), e))
    if "pack" not in doc:
        raise ToolError("%s: [pack]: missing section (REQUIRED)" % rel(path))
    p = Sec(path, "pack", doc["pack"])
    kind = p.raw("kind")
    if kind not in KIND_NAMES:
        p.err("kind", 'must be "species" or "habitat"')
    kind = KIND_NAMES[kind]
    uid_ns = p.raw("uid_ns")
    if not isinstance(uid_ns, str) or not UID_NS_RE.match(uid_ns):
        p.err("uid_ns", "must be 3..47 characters of [a-z0-9._-]")
    name = p.text("name", 1, 23)
    world = p.enum("world", WORLDS)
    legacy_id = None
    if p.has("legacy_id"):
        legacy_id = p.uint("legacy_id", 1, 255 if kind == KIND_SPECIES else 31)
    test_only = p.boolean("test_only") if p.has("test_only") else False
    habitats = []
    if kind == KIND_SPECIES:
        habitats = p.raw("habitats")
        if not isinstance(habitats, list) or not 1 <= len(habitats) <= 4:
            p.err("habitats", "must be a list of 1..4 habitat uid_ns strings")
        for h in habitats:
            if isinstance(h, bool) or not (isinstance(h, int) and 1 <= h <= 0xFFFFFFFF
                                           or isinstance(h, str) and UID_NS_RE.match(h)):
                p.err("habitats", "each entry is a uid_ns string (3..47 of [a-z0-9._-]) or an integer uid")
        if len(set(habitats)) != len(habitats):
            p.err("habitats", "a habitat is listed twice")
    p.finish()
    uid = fnv1a32(uid_ns.encode("ascii"))
    if uid == 0:
        p.err("uid_ns", "hashes to uid 0 (rename the pack)")

    recs = {R_NAME: name.encode("ascii"), R_UID_NS: uid_ns.encode("ascii"), R_WORLD: bytes([world])}
    if legacy_id is not None:
        recs[R_LEGACY] = bytes([legacy_id])
    if test_only:
        recs[R_TEST] = b"\x01"
    sections = SPECIES_SECTIONS if kind == KIND_SPECIES else HABITAT_SECTIONS
    for k in doc:
        if k not in sections:
            raise ToolError("%s: [%s]: unknown section for a %s" % (rel(path), k, "species" if kind == KIND_SPECIES else "habitat"))
    for sec in sections[1:]:
        if sec not in doc:
            if sec not in OPTIONAL_SECTIONS:
                raise ToolError("%s: [%s]: missing section (REQUIRED)" % (rel(path), sec))
            continue
        val = doc[sec]
        if sec in ARRAY_SECTIONS:
            if not isinstance(val, list) or not val or not all(isinstance(t, dict) for t in val):
                raise ToolError("%s: [[%s]]: must be an array of tables" % (rel(path), sec))
        elif not isinstance(val, dict):
            raise ToolError("%s: [%s]: must be a table" % (rel(path), sec))
        rid = SEC_RECORD[sec]
        if rid in FIXED:
            recs[rid] = enc_fixed(Sec(path, sec, val), FIXED[rid][1])
        elif sec == "diet":
            s = Sec(path, sec, val)
            eats = s.strlist("eats", 0, 4)
            bits = 0
            for e in eats:
                if e not in DIET_BITS:
                    s.err("eats", 'unknown food "%s" (%s)' % (e, ", ".join(DIET_BITS)))
                if bits & DIET_BITS[e]:
                    s.err("eats", 'food "%s" listed twice' % e)
                bits |= DIET_BITS[e]
            s.finish()
            recs[rid] = bytes([bits])
        elif sec == "morphs":
            if len(val) > 6:
                raise ToolError("%s: [[morphs]]: at most 6 (is %d)" % (rel(path), len(val)))
            out = bytearray([len(val)])
            for i, m in enumerate(val):
                s = Sec(path, "morphs #%d" % (i + 1), m)
                out += s8(s.text("name", 1, 11)) + u32(s.uint("body", 0, 0xFFFFFF)) + u32(s.uint("accent", 0, 0xFFFFFF))
                s.finish()
            recs[rid] = bytes(out)
        elif sec == "patterns":
            s = Sec(path, sec, val)
            names = s.strlist("by_stage", 4, 4)
            out = bytearray([4])
            for n in names:
                p_ = text_problem(n, 1, 11)
                if p_:
                    s.err("by_stage", '"%s": %s' % (n, p_))
                out += s8(n)
            s.finish()
            recs[rid] = bytes(out)
        elif sec == "goals":
            s = Sec(path, sec, val)
            names = s.strlist("names", 1, 8)
            valid = GOAL_NAMES[world]
            for n in names:
                if n not in valid:
                    s.err("names", 'goal "%s" is not in the %s list (%s)' % (n, "aquarium" if world == 1 else "terrarium", " ".join(valid)))
            if len(set(names)) != len(names):
                s.err("names", "a goal is listed twice")
            s.finish()
            recs[rid] = bytes([len(names)]) + b"".join(s8(n) for n in names)
        elif sec == "hides":
            if len(val) > 4:
                raise ToolError("%s: [[hides]]: at most 4 (is %d)" % (rel(path), len(val)))
            out = bytearray([len(val)])
            for i, h in enumerate(val):
                s = Sec(path, "hides #%d" % (i + 1), h)
                out.append(s.enum("kind", HIDE_KINDS))
                for k in ("u", "t_lit_c", "t_dark_c"):
                    out += u32(s.f32(k))
                s.finish()
            recs[rid] = bytes(out)
        elif sec == "rocks":
            if len(val) > 4:
                raise ToolError("%s: [[rocks]]: at most 4 (is %d)" % (rel(path), len(val)))
            out = bytearray([len(val)])
            for i, r in enumerate(val):
                s = Sec(path, "rocks #%d" % (i + 1), r)
                shelf = s.boolean("shelf")
                pts = s.raw("points")
                if not isinstance(pts, list) or not 2 <= len(pts) <= 4 or not all(isinstance(q, list) and len(q) == 2 for q in pts):
                    s.err("points", "must be 2..4 [u, v] pairs")
                out += bytes([len(pts), 1 if shelf else 0])
                for q in pts:
                    for x in q:
                        out += u32(s._bits("points", x))
                s.finish()
            recs[rid] = bytes(out)
        elif sec == "ramps":
            if len(val) > 6:
                raise ToolError("%s: [[ramps]]: at most 6 (is %d)" % (rel(path), len(val)))
            out = bytearray([len(val)])
            for i, r in enumerate(val):
                s = Sec(path, "ramps #%d" % (i + 1), r)
                out += bytes([s.uint("lo", 0, 255), s.uint("hi", 0, 255)]) + u32(s.f32("u_lo")) + u32(s.f32("u_hi"))
                s.finish()
            recs[rid] = bytes(out)
    if kind == KIND_SPECIES:
        recs[R_HABITATS] = None                              # placeholder: resolved by encode_blob()
    return Pack(path, kind, uid_ns, name, world, legacy_id, test_only, habitats, recs)


def encode_blob(pack, habitat_uids):
    """pack.blob holds the records dict until the habitat uids are known; returns the file's bytes"""
    recs = dict(pack.blob)
    if pack.kind == KIND_SPECIES:
        recs[R_HABITATS] = bytes([len(habitat_uids)]) + b"".join(u32(u) for u in habitat_uids)
    body = b"".join(struct.pack("<HH", rid | (REQUIRED if RECORDS[rid][1] else 0), len(recs[rid])) + recs[rid]
                    for rid in sorted(recs))
    if not 0 < len(body) <= BODY_MAX:
        raise ToolError("%s: body is %d bytes (limit %d)" % (rel(pack.path), len(body), BODY_MAX))
    return struct.pack("<4sBBHIII", b"PTPK", FMT, pack.kind, 0, pack.uid, len(body), zlib.crc32(body) & 0xFFFFFFFF) + body


def habitat_uid(h):
    return h if isinstance(h, int) else fnv1a32(h.encode("ascii"))


# ------------------------------------------------------------------------------------------------------------- decoding


class PackError(ToolError):
    pass


class F32:
    def __init__(self, bits):
        self.bits = bits


class Hex(int):
    width = 6


def hexw(v, w):
    h = Hex(v)
    h.width = w
    return h


class Rd:
    """a reader over one record's value; every overrun and every trailing byte is an error"""

    def __init__(self, fail, buf):
        self.fail, self.buf, self.pos = fail, buf, 0

    def take(self, n):
        if self.pos + n > len(self.buf):
            self.fail("is %d bytes, too short" % len(self.buf))
        b = self.buf[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def f32(self):
        bits = self.u32()
        if (bits >> 23) & 0xFF == 0xFF:
            self.fail("holds a non-finite float (0x%08x)" % bits)
        return F32(bits)

    def f32s(self, n):
        return [self.f32() for _ in range(n)]

    def text(self, lo, hi, what):
        n = self.u8()
        s = self.take(n).decode("latin-1")
        p = text_problem(s, lo, hi)
        if p:
            self.fail("%s %s" % (what, p))
        return s

    def done(self):
        if self.pos != len(self.buf):
            self.fail("is %d bytes, its layout says %d" % (len(self.buf), self.pos))


def reverse(m, v, fail, what):
    for k, x in m.items():
        if x == v:
            return k
    fail("%s %d is out of range" % (what, v))


def known_habitats():
    """uid -> uid_ns for the habitat packs in the repo's packs/ (so a dumped species can name its habitats)"""
    m = {}
    for f in sorted((ROOT / "packs").glob("*.ptpk")):
        try:
            d = f.read_bytes()
            _magic, _fmt, kind, _flags, _uid, ln, _crc = struct.unpack_from("<4sBBHIII", d)
            pos = 20
            while kind == KIND_HABITAT and pos + 4 <= 20 + ln <= len(d):      # a light walk: the UID_NS record
                tag, rlen = struct.unpack_from("<HH", d, pos)
                if tag & 0x7FFF == R_UID_NS:
                    ns = d[pos + 4:pos + 4 + rlen].decode("ascii")
                    m[fnv1a32(ns.encode("ascii"))] = ns
                    break
                pos += 4 + rlen
        except (OSError, struct.error, UnicodeDecodeError):
            continue
    return m


def decode(data, name):
    """validate one .ptpk and return (header facts, [(header, is_array, [(key, value)])], notes); PackError on any refusal"""
    def refuse(why):
        raise PackError("%s: %s" % (name, why))

    if len(data) < 20:
        refuse("no header (%d bytes, need 20)" % len(data))
    magic, fmt, kind, flags, uid, ln, crc = struct.unpack_from("<4sBBHIII", data)
    if magic != b"PTPK":
        refuse("wrong magic %r (not a PTPK file)" % magic)
    if fmt == 0 or fmt > FMT:
        refuse("fmt %d is not supported (this tool reads fmt %d)" % (fmt, FMT))
    if kind == 3:
        refuse("kind 3 is a model manifest (reserved; not read)")
    if kind not in (KIND_SPECIES, KIND_HABITAT):
        refuse("unknown kind %d" % kind)
    if flags:
        refuse("flags 0x%04x set (every flag bit is reserved)" % flags)
    if uid == 0:
        refuse("uid is 0")
    if not 0 < ln <= BODY_MAX:
        refuse("len %d is out of range 1..%d" % (ln, BODY_MAX))
    if len(data) != 20 + ln:
        refuse("file is %d bytes, the header says %d" % (len(data), 20 + ln))
    body = data[20:]
    if zlib.crc32(body) & 0xFFFFFFFF != crc:
        refuse("CRC mismatch (header 0x%08x, body 0x%08x)" % (crc, zlib.crc32(body) & 0xFFFFFFFF))

    raw, unknown, notes = {}, [], []
    pos = 0
    while pos < len(body):
        if len(body) - pos < 4:
            refuse("truncated: %d stray bytes after the last record" % (len(body) - pos))
        tag, rlen = struct.unpack_from("<HH", body, pos)
        pos += 4
        if pos + rlen > len(body):
            refuse("truncated: record 0x%04x claims %d bytes, %d left" % (tag, rlen, len(body) - pos))
        val = body[pos:pos + rlen]
        pos += rlen
        rid = tag & 0x7FFF
        info = RECORDS.get(rid)
        if info and info[2] in (0, kind):
            if rid in raw:
                refuse("record %s appears twice" % info[0])
            raw[rid] = val
            if bool(tag & REQUIRED) != info[1]:
                notes.append("record %s carries the REQUIRED bit %s; build writes it %s"
                             % (info[0], "set" if tag & REQUIRED else "clear", "set" if info[1] else "clear"))
        elif tag & REQUIRED:
            refuse("unknown REQUIRED record 0x%04x (%d bytes)" % (tag, rlen))
        else:
            unknown.append("# unknown tag 0x%04x (optional, %d bytes): %s" % (tag, rlen, val.hex()))

    def reader(rid):
        nm = RECORDS[rid][0]
        return Rd(lambda why: refuse("record %s %s" % (nm, why)), raw[rid]), (lambda why: refuse("record %s %s" % (nm, why)))

    for rid in (R_NAME, R_UID_NS, R_WORLD):
        if rid not in raw:
            refuse("record %s is missing" % RECORDS[rid][0])
    pack = []
    # common
    r, fail = reader(R_NAME)
    nm = raw[R_NAME].decode("latin-1")
    p = text_problem(nm, 1, 23)
    if p:
        fail("%s" % p)
    r, fail = reader(R_UID_NS)
    uid_ns = raw[R_UID_NS].decode("latin-1")
    if not UID_NS_RE.match(uid_ns):
        fail("%r is not 3..47 characters of [a-z0-9._-]" % uid_ns)
    if fnv1a32(raw[R_UID_NS]) != uid:
        fail("%s hashes to 0x%08x, the header says 0x%08x" % (uid_ns, fnv1a32(raw[R_UID_NS]), uid))
    r, fail = reader(R_WORLD)
    if len(raw[R_WORLD]) != 1 or raw[R_WORLD][0] not in (1, 2):
        fail("must be one byte, 1 aquarium or 2 terrarium")
    world = raw[R_WORLD][0]
    head = [("kind", "species" if kind == KIND_SPECIES else "habitat"), ("uid_ns", uid_ns), ("name", nm),
            ("world", reverse(WORLDS, world, fail, "world"))]
    if R_LEGACY in raw:
        r, fail = reader(R_LEGACY)
        v = r.u8()
        r.done()
        if not 1 <= v <= (255 if kind == KIND_SPECIES else 31):
            fail("%d is out of range" % v)
        head.append(("legacy_id", v))
    if R_TEST in raw:
        r, fail = reader(R_TEST)
        if r.u8() != 1:
            fail("must be 1")
        r.done()
        head.append(("test_only", True))
    if kind == KIND_SPECIES:
        r, fail = reader(R_HABITATS) if R_HABITATS in raw else refuse("record HABITATS is missing")
        n = r.u8()
        if not 1 <= n <= 4:
            fail("count %d is out of range 1..4" % n)
        names = known_habitats()
        hs = []
        for _ in range(n):
            u = r.u32()
            hs.append(names.get(u) or hexw(u, 8))
        r.done()
        head.append(("habitats", hs))
    pack.append(("[pack]", False, head))

    secs = SPECIES_SECTIONS if kind == KIND_SPECIES else HABITAT_SECTIONS
    for sec in secs[1:]:
        rid = SEC_RECORD[sec]
        if rid not in raw:
            if sec not in OPTIONAL_SECTIONS:
                refuse("record %s is missing" % RECORDS[rid][0])
            continue
        r, fail = reader(rid)
        if rid in FIXED:
            size = sum(1 if t in ("enum", "bool", "u8") else 4 * a for _k, t, a in FIXED[rid][1])
            if len(raw[rid]) != size:
                fail("is %d bytes, its layout says %d" % (len(raw[rid]), size))
            d = []
            for key, typ, arg in FIXED[rid][1]:
                if typ == "f32":
                    d.append((key, r.f32() if arg == 1 else r.f32s(arg)))
                elif typ == "enum":
                    d.append((key, reverse(arg, r.u8(), fail, key)))
                elif typ == "bool":
                    v = r.u8()
                    if v > 1:
                        fail("%s %d is not 0 or 1" % (key, v))
                    d.append((key, bool(v)))
                else:
                    d.append((key, r.u8()))
            r.done()
            pack.append(("[%s]" % sec, False, d))
        elif sec == "diet":
            v = r.u8()
            r.done()
            if v & ~0xF:
                fail("diet bits 0x%02x: only 1, 2, 4, 8 exist" % v)
            pack.append(("[diet]", False, [("eats", [k for k, b in DIET_BITS.items() if v & b])]))
        elif sec == "morphs":
            n = r.u8()
            if not 1 <= n <= 6:
                fail("count %d is out of range 1..6" % n)
            for _ in range(n):
                mn = r.text(1, 11, "morph name")
                b, a = r.u32(), r.u32()
                if b > 0xFFFFFF or a > 0xFFFFFF:
                    fail("colour 0x%08x is above 0xffffff" % max(a, b))
                pack.append(("[[morphs]]", True, [("name", mn), ("body", hexw(b, 6)), ("accent", hexw(a, 6))]))
            r.done()
        elif sec == "patterns":
            n = r.u8()
            if n != 4:
                fail("count %d, must be 4" % n)
            names = [r.text(1, 11, "pattern name") for _ in range(4)]
            r.done()
            pack.append(("[patterns]", False, [("by_stage", names)]))
        elif sec == "goals":
            n = r.u8()
            if not 1 <= n <= 8:
                fail("count %d is out of range 1..8" % n)
            names = []
            for _ in range(n):
                g = r.text(1, 15, "goal name")
                if g not in GOAL_NAMES[world]:
                    fail("goal %s is not in the %s's list" % (g, "aquarium" if world == 1 else "terrarium"))
                if g in names:
                    fail("goal %s is listed twice" % g)
                names.append(g)
            r.done()
            pack.append(("[goals]", False, [("names", names)]))
        elif sec == "hides":
            n = r.u8()
            if n > 4:
                fail("count %d is out of range 0..4" % n)
            if n == 0:
                notes.append("record HIDES holds no hides; build leaves the record out")
            for _ in range(n):
                k = reverse(HIDE_KINDS, r.u8(), fail, "kind")
                pack.append(("[[hides]]", True, [("kind", k), ("u", r.f32()), ("t_lit_c", r.f32()), ("t_dark_c", r.f32())]))
            r.done()
        elif sec == "rocks":
            n = r.u8()
            if n > 4:
                fail("count %d is out of range 0..4" % n)
            if n == 0:
                notes.append("record ROCKS holds no rocks; build leaves the record out")
            for _ in range(n):
                npts, shelf = r.u8(), r.u8()
                if not 2 <= npts <= 4:
                    fail("a rock has %d points, 2..4 allowed" % npts)
                if shelf > 1:
                    fail("shelf %d is not 0 or 1" % shelf)
                pts = [r.f32s(2) for _ in range(npts)]
                pack.append(("[[rocks]]", True, [("shelf", bool(shelf)), ("points", pts)]))
            r.done()
        elif sec == "ramps":
            n = r.u8()
            if n > 6:
                fail("count %d is out of range 0..6" % n)
            if n == 0:
                notes.append("record RAMPS holds no ramps; build leaves the record out")
            for _ in range(n):
                lo, hi = r.u8(), r.u8()
                pack.append(("[[ramps]]", True, [("lo", lo), ("hi", hi), ("u_lo", r.f32()), ("u_hi", r.f32())]))
            r.done()
    return uid, len(data), pack, notes, unknown


def tv(v):
    if isinstance(v, F32):
        return f32_text(v.bits)
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, Hex):
        return "0x%0*x" % (v.width, v)
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"')
    return "[" + ", ".join(tv(x) for x in v) + "]"


def dump_text(data, name):
    uid, size, pack, notes, unknown = decode(data, name)
    out = ["# %s: uid 0x%08x, %d bytes (tools/ptpk.py dump; `ptpk.py build` of this text gives the file back)" % (Path(name).name, uid, size)]
    for head, _is_arr, items in pack:
        out.append("")
        out.append(head)
        for k, v in items:
            out.append("%s = %s" % (k, tv(v)))
    if notes or unknown:
        out.append("")
        out += ["# note: " + n for n in notes] + unknown
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------------------------------------------------- commands


def cmd_build(src, out):
    pk = compile_source(src)
    habs = [habitat_uid(h) for h in pk.habitats]
    pk.file = encode_blob(pk, habs)
    dest = Path(out) if out else Path(pk.uid_ns + ".ptpk")
    dest.write_bytes(pk.file)
    print("ptpk: %s: %s uid 0x%08x, %d bytes" % (dest, pk.uid_ns, pk.uid, len(pk.file)))


def read_file(path):
    try:
        return Path(path).read_bytes()
    except OSError as e:
        raise ToolError("%s: cannot read: %s" % (path, e))


def cmd_dump(path):
    sys.stdout.write(dump_text(read_file(path), str(path)))


def cmd_verify(path):
    """exit 1 on a pack the loader would refuse for its bytes (header, length, CRC, record walk, record layouts)"""
    try:
        uid, size, pack, _notes, _unknown = decode(read_file(path), str(path))
    except PackError as e:
        sys.stderr.write("ptpk: %s\n" % e)
        return 1
    head = dict(pack[0][2])
    print("ptpk: %s: ok, %s %s uid 0x%08x, %d bytes" % (path, head["kind"], head["uid_ns"], uid, size))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog="ptpk.py", description="PTPK pack tool (docs/ptpk.md): author a new pack, read or check one")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="compile one TOML source into a .ptpk")
    b.add_argument("src")
    b.add_argument("-o", "--out", help="output file (default ./<uid_ns>.ptpk)")
    d = sub.add_parser("dump", help="check a .ptpk and print it as a source")
    d.add_argument("file")
    v = sub.add_parser("verify", help="check a .ptpk's header, length, CRC and records; exit 1 when it is bad")
    v.add_argument("file")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "build":
            cmd_build(a.src, a.out)
        elif a.cmd == "dump":
            cmd_dump(a.file)
        else:
            return cmd_verify(a.file)
    except ToolError as e:
        sys.stderr.write("ptpk: %s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

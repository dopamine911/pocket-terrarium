#!/usr/bin/env python3
"""gen_terr_icons.py - draw the terrarium's variants of the fish-specific badges.

Writes assets/icons/<name>_terr.png beside each aquarium icon it replaces in the
terrarium's bin (gen_icons.py then bakes them; common/icons.h's WORLD_ICON picks
<name>_terr in the terrarium and <name> in the aquarium, and each bin carries
only its own art). Same size as the original, the same style: hard-edged pixel
art, three-tone shading, a dark outline. The geckos, crickets, rocks and sand
are character maps below; where an original's non-fish part still fits (the
hand, the glass, the moon, the coin, the wiper), it is cropped from the
original so the pair stays alike.

    python3 tools/gen_terr_icons.py && python3 tools/gen_icons.py
"""
import math
import os
from PIL import Image, ImageOps

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "assets", "icons")

OUTLINE = (26, 20, 18, 255)
PAL = {
    # the leopard gecko (render_lizard.c's NORMAL morph: yellow ground, dark spots, pale belly)
    "H": 0xf9e08a, "Y": 0xf1c447, "S": 0xc28d2c, "D": 0x3a2733, "B": 0xf3eee0, "b": 0xcfc4ae,
    "E": 0x110f0e, "W": 0xffffff, "s": 0x8a6a1e,
    # the cricket
    "c": 0xd9a35c, "C": 0xa86f32, "k": 0x6e4420, "A": 0x4a2e16,
    # sandstone and sand (render_lizard.c P_SAND for the sand's shade)
    "r": 0xe4bc8c, "R": 0xc28c5c, "q": 0x8e5e3a, "Q": 0x5e3a24, "n": 0xead096, "N": 0xd2aa6e, "m": 0xb48c60,
    # the lamp, sparkles, a sleeping z
    "l": 0xfff6c0, "L": 0xffd24a, "o": 0xf09a2a, "w": 0xfff4d0,
    "z": OUTLINE[:3],
    # an amber gecko iris's dark rim
    "u": 0xb06a1a,
    # the tangerine morph (the second gecko in a pair)
    "t": 0xffc882, "T": 0xf39a3c, "O": 0xb8641e,
    # a succulent: green, pink-tipped
    "g": 0xa6d68e, "G": 0x63a85e, "v": 0x3a6e44, "p": 0xf08ca8,
    # the desert rose (gypsum): a glint, highlight, light, mid, shadow, the gaps between the blades
    "0": 0xfff2e2, "1": 0xf6dcb8, "2": 0xe9b98c, "3": 0xcf9466, "4": 0x9a6040, "5": 0x6a3c26,
}


def rgb(v):
    return v if isinstance(v, tuple) else ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def sprite(rows, outline=True):
    """a character map -> RGBA, with a 1 px outline round it (4-neighbour) when asked"""
    h, w = len(rows), max(len(r) for r in rows)
    pad = 1 if outline else 0
    im = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    px = im.load()
    for j, row in enumerate(rows):
        for i, ch in enumerate(row):
            if ch != ".":
                px[i + pad, j + pad] = rgb(PAL[ch]) + (255,)
    if outline:
        solid = {(i, j) for j in range(im.height) for i in range(im.width) if px[i, j][3]}
        for i, j in solid:
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                p = (i + di, j + dj)
                if p not in solid and 0 <= p[0] < im.width and 0 <= p[1] < im.height:
                    px[p] = OUTLINE
    return im


def mirror(im):
    return ImageOps.mirror(im)


def canvas(size=32):
    return Image.new("RGBA", (size, size), (0, 0, 0, 0))


def put(dst, im, x, y):
    """composite im onto dst at (x, y), clipped to dst"""
    layer = Image.new("RGBA", dst.size, (0, 0, 0, 0))
    layer.paste(im, (x, y))
    dst.alpha_composite(layer)


def crop(name, box):
    return Image.open(os.path.join(ICONS, name + ".png")).convert("RGBA").crop(box)


def tilt(rows, k):
    """a character map sheared: column i moved down round(i * k) rows (k < 0: up) - a lean that keeps hard pixels"""
    w = max(len(r) for r in rows)
    off = [round(i * k) for i in range(w)]
    lo = min(off)
    off = [o - lo for o in off]
    out = [["."] * w for _ in range(len(rows) + max(off))]
    for j, row in enumerate(rows):
        for i, ch in enumerate(row):
            if ch != ".":
                out[j + off[i]][i] = ch
    return ["".join(r) for r in out]


# ---- the sprites (facing right unless named otherwise) ----
GECKO = [                       # 26 x 10: a leopard gecko walking, the head up, the tail thinning to its tip
    "....................HHH...",
    ".............HHHHH.HYYYHH.",
    ".........HHDHYDYYDHHYDYYYH",
    ".....HHDHYYDYYYDYYYDYYYEWY",
    "HHHHYDYYDYYYYDYYYDYYYYYEEY",
    ".SSSSDYYYYDYYYYDYYYDYYYYYs",
    "......SSSSBBBBBBBBBSSBBBb.",
    ".........SYY......SYY.....",
    "........SYY........SYY....",
    ".......SS.S.........S.SS..",
]
GECKO_SMALL = [                 # 16 x 7
    "...........HHH..",
    "......HHDH.HYYH.",
    "..HHDHYYYDHYYYEW",
    "HHYYYYDYYYYDYYEY",
    ".SSSSBBBBBBSBBBs",
    "....SY.....SY...",
    "...SS.....S..SS.",
]
GECKO_HEAD = [                  # 10 x 7: a head and shoulders, out of a hide
    "......HHH.",
    "...HHHYYYH",
    "..HYDYYYYH",
    ".HYYYYYYEW",
    "HYYYDYYYEE",
    "SSYYYYYYYs",
    ".SSBBBBBb.",
]
# asleep: the walker lying flat, the legs tucked under, the eye a shut line
GECKO_ASLEEP = [r.replace("W", "Y").replace("E", "Y") for r in GECKO[:4]] + [GECKO[4].replace("E", "z")] + GECKO[5:7]
CRICKET = [                     # 18 x 9, facing left: long antennae, the jumping hind leg's knee up over the back
    "AA................",
    "..AA.........kk...",
    "....A.......kCCk..",
    "....kkk....kCCkCk.",
    "...kEcckkkkCCk..k.",
    "...kccccccCCCkk..k",
    "....kCCCCCCCCk...k",
    ".....k.k..k......k",
    "....k..k...k......",
]
ROCK = [                        # 22 x 9: sandstone, flat-topped for basking
    ".....rrrrrrrrrr.......",
    "...rrRRRRRRRRRRrrr....",
    "..rRRRRRRRRRRRRRRRrr..",
    ".rRRRRqRRRRRRRRRRRRRr.",
    ".RRRRRRRRRRRRRqRRRRRR.",
    "RRRqRRRRRRRRRRRRRRRRRq",
    "qRRRRRRRRRqRRRRRRRRRqq",
    "qqqRRRRRRRRRRRRRRRqqqq",
    ".qqqqqqqqqqqqqqqqqqqq.",
]
HIDE = [                        # 26 x 15: a rock cave, its mouth dark
    ".........rrrrrrr..........",
    "......rrrRRRRRRRrrr.......",
    "....rrRRRRRRRRRRRRRrr.....",
    "...rRRRRRRqRRRRRRRRRRr....",
    "..rRRRRRRRRRRRRRRRRRRRr...",
    ".rRRRRRRRRRRRRRRRRqRRRRr..",
    ".RRRRRqqqqqqqqqRRRRRRRRR..",
    "rRRRqQQQQQQQQQQQqRRRRRRRr.",
    "RRRqQQQQQQQQQQQQQqRRRqRRR.",
    "RRqQQQQQQQQQQQQQQQqRRRRRRq",
    "RRqQQQQQQQQQQQQQQQqRRRRRqq",
    "qRqQQQQQQQQQQQQQQQqRRRRqqq",
    "qqqQQQQQQQQQQQQQQQqqqqqqqq",
    "nnnnnnnnnnnnnnnnnnnnnnnnnn",
    "NNNNmNNNNNNNNNNmNNNNNNNNmN",
]
SAND = [                        # 32 x 3: a strip of desert floor
    "..nnnnnnnnnnnnnnnnnnnnnnnnnnnn..",
    "nnnnNNnnnnnnnnNNnnnnnnnnnNNnnnnn",
    "NNmNNNNNNmNNNNNNNNNmNNNNNNNNmNNN",
]
MOUND = [                       # 16 x 5: a dug-up heap of sand
    ".....nnnnnn.....",
    "...nnnNNnnnnn...",
    ".nnNNNNNNNNNnnn.",
    "nNNNmNNNNNmNNNNn",
    "NNmNNNNmNNNNNmNN",
]
LAMP = [                        # 9 x 9: the basking lamp's glow
    "....l....",
    ".l..L..l.",
    "..lLLLl..",
    "..LLlLL..",
    "lLLlllLLl",
    "..LLlLL..",
    "..lLLLl..",
    ".l..L..l.",
    "....l....",
]
SPARKLE = [
    "..w..",
    "..w..",
    "wwlww",
    "..w..",
    "..w..",
]
GRAINS = ["n", "N", "m"]


def grain(dst, x, y, ch):
    put(dst, sprite([ch], outline=False), x, y)


def first_meal():                       # first meal from you: the hand offers a cricket, the gecko reaches up
    im = canvas()
    put(im, crop("ms_first_meal", (12, 2, 26, 14)), 12, 1)       # the pinching hand
    put(im, sprite(CRICKET), 1, 10)
    put(im, sprite(tilt(GECKO, -0.2)), 5, 15)
    return im


def first_hold_approach():              # a finger on the glass, the gecko come to it
    im = canvas()
    put(im, crop("ms_first_hold_approach", (0, 5, 17, 28)), 0, 5)  # the finger and the glass
    put(im, mirror(sprite(GECKO_SMALL)), 16, 14)
    return im


def first_hide():                       # MS_FIRST_GRASS in the terrarium: safe in a hide, peeking out
    im = canvas()
    put(im, sprite(HIDE), 2, 11)
    put(im, sprite(GECKO_HEAD), 8, 19)
    return im


def first_bask():                       # MS_FIRST_BUBBLES in the terrarium: up on the basking spot
    im = canvas()
    put(im, sprite(LAMP, outline=False), 20, 0)
    put(im, sprite(ROCK), 4, 21)
    put(im, sprite(GECKO), 1, 11)
    return im


def first_follow():                     # two geckos, one after the other
    im = canvas()
    put(im, sprite(GECKO_SMALL), 0, 6)
    put(im, sprite(GECKO_SMALL), 13, 18)
    return im


def first_dig():                        # MS_FIRST_DART in the terrarium: a dig, the sand flying
    im = canvas()
    put(im, sprite(tilt(GECKO, 0.3)), 0, 6)
    put(im, sprite(MOUND), 16, 26)
    for x, y, ch in ((3, 24, "n"), (5, 21, "N"), (2, 19, "m"), (7, 26, "n"), (1, 27, "N"), (9, 23, "n"), (4, 16, "n")):
        grain(im, x, y, ch)
    return im


def first_feeding():                    # the tank's first feeding: crickets
    im = canvas()
    put(im, sprite(CRICKET), 2, 4)
    put(im, mirror(sprite(CRICKET)), 15, 14)
    put(im, sprite(SAND), 0, 29)
    return im


def first_glass_cleaning():             # the wiper through the smudge, a gecko behind the glass
    glass = crop("ms_first_glass_cleaning", (0, 0, 32, 32))
    glass.paste((0, 0, 0, 0), (19, 10, 32, 19))                  # the fish gone (its box clear of the wiper)
    im = canvas()
    put(im, sprite(GECKO_SMALL), 14, 9)                         # behind the glass: the wiper and the film over its tail
    im.alpha_composite(glass)
    return im


def first_quiet_night():                # asleep on the sand under the moon
    im = canvas()
    put(im, crop("ms_first_quiet_night", (14, 1, 26, 15)), 14, 1)  # the moon
    put(im, sprite(SAND), 0, 29)
    put(im, sprite(GECKO_ASLEEP), 3, 21)
    put(im, sprite(["ww.", ".w.", "ww."], outline=False), 27, 15)
    put(im, sprite(["lll", "..l", ".l.", "lll"], outline=False), 28, 10)
    return im


def first_play_session():               # two geckos round each other, kicking up dust
    im = canvas()
    put(im, sprite(GECKO_SMALL), 0, 3)
    put(im, mirror(sprite(GECKO_SMALL)), 14, 20)
    for x, y, ch in ((20, 9, "n"), (23, 11, "N"), (25, 14, "n"), (8, 22, "N"), (5, 19, "n"), (10, 25, "m"), (27, 7, "m")):
        grain(im, x, y, ch)
    return im


def changed_someone():                  # a gecko changed: its tail gone another colour, a sparkle
    im = canvas()
    body = sprite(GECKO)
    px = body.load()
    for j in range(body.height):
        for i in range(10):             # the tail's half: tinted toward the cool blue of the original's other half
            r, g, b, a = px[i, j]
            if a and (r, g, b, a) != OUTLINE:
                px[i, j] = (int(r * 0.35), int(g * 0.65 + 40), min(255, int(b * 0.6 + 150)), a)
    put(im, body, 3, 14)
    put(im, sprite(SPARKLE, outline=False), 25, 4)
    put(im, sprite(["w"], outline=False), 22, 10)
    return im


def first_sale():                       # a gecko sold: the gecko and the coin
    im = canvas()
    put(im, sprite(GECKO_SMALL), 1, 5)
    put(im, crop("ms_first_sale", (15, 15, 31, 31)), 15, 15)
    return im


# ---- the card's icons (16 and 24 px) ----
GECKO_MINI = [                  # 14 x 7: GECKO_SMALL two columns shorter, for the 16 px icons
    ".........HHH..",
    ".....HDH.HYYH.",
    ".HHDHYYDHYYYEW",
    "HYYYYDYYYDYYEY",
    ".SSSBBBBBSBBBs",
    "...SY....SY...",
    "..SS....S..SS.",
]
GECKO_STRIDE = [                # 12 x 8: mid-stride, the legs flung fore and aft
    "........HHH.",
    "....HHDHYYYH",
    "..HHYDYYYYEW",
    "HHYDYYDYYYEY",
    ".SSBBBBBSBBs",
    "...SY...SY..",
    "..S.....S...",
    ".SS......SS.",
]
ROCK_WIDE = [                   # 13 x 7: a sandstone boulder to hide behind
    "..rrrrrrr....",
    ".rRRRRRRRrrr.",
    "rRRRRqRRRRRRr",
    "RRRRRRRRRqRRR",
    "RqRRRRRRRRRRq",
    "qRRRRRRqRRRqq",
    ".qqqqqqqqqqq.",
]
EYE_OPEN = [                    # 13 x 7: a leopard gecko's eye, wide: amber iris, slit pupil, a glint
    "....uuuuu....",
    "..uuooEoouu..",
    ".uoWWoEoooou.",
    "uoWoooEooooou",
    ".uoLLLELLLou.",
    "..uLLLELLLu..",
    "....uuuuu....",
]
EYE_NARROW = [                  # 13 x 5: the same eye, the lids drawn in to a slit
    "...zzzzzzz...",
    ".zzuuuEuuuzz.",
    "zuoWooEoooouz",
    ".zzLLLELLLzz.",
    "...zzzzzzz...",
]
STAR = [".L.", "LlL", ".L."]
CR_BODY = [                     # 19 x 9, facing left: head, saddle, folded wings, the legs under
    "..cc...cccc........",
    ".cccc.kcccccccc....",
    "ccEcckcccCccccccc..",
    "cccccCkCCCCCCCCCCC.",
    "CccCCkCCkCCkCCkCCCk",
    ".CCk.kkCCCCCCCCCCk.",
    "..C..C.kkkkkkkkk...",
    ".C...C..C..........",
    "C...C....C.........",
]
CR_FEMUR = [                    # the jumping leg's thigh, the knee up over the back
    "....cc",
    "...cCk",
    "...cCk",
    "..ccCk",
    "..cCkk",
    ".ccCCk",
    ".cCkCk",
    "ccCCk.",
    "cCCk..",
]
CR_TIBIA = ["C..", "C..", "C..", ".C.", ".C.", ".C.", ".C.", ".C.", "..C", "..C", "..C", "..C"]


def line(dst, pts, ch):
    """a 1 px line through pts, no outline (antennae)"""
    px = dst.load()
    col = rgb(PAL[ch]) + (255,)
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        n = max(abs(x1 - x0), abs(y1 - y0))
        for t in range(n + 1):
            px[round(x0 + (x1 - x0) * t / n), round(y0 + (y1 - y0) * t / n)] = col


def tangerine(rows):
    return [r.replace("H", "t").replace("Y", "T").replace("S", "O") for r in rows]


def hunger():                           # the food meter: a cricket, the hind leg cocked to jump
    im = canvas(24)
    line(im, [(3, 11), (3, 8), (4, 5), (6, 3), (9, 1), (13, 0), (17, 0)], "c")
    line(im, [(5, 11), (5, 8), (7, 5), (10, 3), (14, 2)], "C")
    put(im, sprite(CR_TIBIA), 19, 5)
    put(im, sprite(CR_BODY), 1, 10)
    put(im, sprite(CR_FEMUR), 13, 5)
    return im


def shy():                              # a gecko peeking over a rock
    im = canvas(16)
    put(im, sprite(GECKO_HEAD), 3, 0)
    put(im, sprite(ROCK_WIDE), 0, 7)
    return im


def bold():                             # a gecko striding out, the dust streaking behind
    im = canvas(16)
    put(im, sprite(tilt(GECKO_STRIDE, -0.12)), 3, 3)
    for y, x0, x1 in ((5, 0, 3), (8, 0, 2), (11, 1, 4)):
        put(im, sprite(["n" * (x1 - x0)], outline=False), x0, y)
    return im


def solo():
    im = canvas(16)
    put(im, sprite(GECKO_MINI), 0, 4)
    return im


def social():                           # two geckos, a tangerine one behind and above
    im = canvas(16)
    put(im, sprite(tangerine(GECKO_MINI)), 1, 1)
    put(im, sprite(GECKO_MINI), 0, 7)
    return im


def cautious():
    im = canvas(16)
    put(im, sprite(EYE_NARROW), 1, 7)
    return im


def curious():
    im = canvas(16)
    put(im, sprite(EYE_OPEN), 0, 6)
    put(im, sprite(STAR), 11, 0)
    return im


# ---- the desert rose: the terrarium's currency ----
def unit(v):
    n = math.hypot(*v)
    return (v[0] / n, v[1] / n)


def ring_blades(cx, cy, rings, squash, lift):
    """blades round concentric rings (outer first): (x, y, half-length, half-width, angle, outward unit)"""
    out = []
    for R, n, a, b, phase, twist in rings:
        for k in range(n):
            t = phase + 2 * math.pi * k / n
            bx, by = cx + R * math.cos(t), cy + R * squash * math.sin(t) - lift * (1 - R / rings[0][0])
            ang = math.degrees(math.atan2(squash * math.cos(t), -math.sin(t))) + twist * math.sin(3 * k + R)
            out.append((bx, by, a, b, ang, unit((bx - cx, (by - cy) / squash + 1e-6))))
    return out


def rose(size, cx, cy, rings, squash, lift, pointy=1.3, light=(-0.5, -0.85)):
    """a rosette of flat crystal blades as a character map: each blade a pointed lens, later ones on top;
    its outer rim glints where it faces the light, its inner edge is a dark gap"""
    blades = ring_blades(cx, cy, rings, squash, lift)
    owner, masks = {}, []
    for i, (bx, by, a, b, ang, rd) in enumerate(blades):
        ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
        m = {}
        for y in range(size):
            for x in range(size):
                dx, dy = x + 0.5 - bx, y + 0.5 - by
                u, v = (dx * ca + dy * sa) / a, (-dx * sa + dy * ca) / b
                if abs(u) <= 1 and abs(v) <= (1 - u * u) ** pointy:
                    m[(x, y)] = (dx * rd[0] + dy * rd[1]) / b
        masks.append(m)
        for p in m:
            owner[p] = i
    lx, ly = unit(light)
    rmax = rings[0][0]
    rows = []
    for y in range(size):
        row = ""
        for x in range(size):
            i = owner.get((x, y), -1)
            if i < 0:
                row += "."
                continue
            m, rd = masks[i], blades[i][5]
            c, rim = None, None
            for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if (x + ddx, y + ddy) not in m:
                    out = ddx * rd[0] + ddy * rd[1]
                    if out > 0.3:
                        rim = max(rim, ddx * lx + ddy * ly) if rim is not None else ddx * lx + ddy * ly
                    elif out < -0.3 and c is None:
                        c = "5"
            if rim is not None:
                c = "0" if rim > 0.3 else "1" if rim > -0.5 else "2"
            if c is None:
                vr = m[(x, y)]
                c = "2" if vr > 0.55 else "3" if vr > 0 else "4"
                lit = ((x - cx) * lx + (y - cy) * ly) / rmax
                if lit < -0.3:
                    c = {"2": "3", "3": "4", "4": "4"}[c]
                elif lit > 0.4 and c == "3":
                    c = "2"
            row += c
        rows.append(row)
    return rows


ROSE_16 = [                     # 14 x 12, the price glyph: the same rosette, a few blades
    ".....0000.....",
    "..0002222000..",
    ".024544005430.",
    "02240002304330",
    "02402000030432",
    "02405040253442",
    "24503402432442",
    "02440342324442",
    "03345022254442",
    ".034544445443.",
    "..3344444433..",
    ".....4444.....",
]
ROSE_RINGS_64 = [(23, 10, 9.5, 5.5, 0.2, 12), (16, 8, 8.5, 5, 0.6, 12), (9.5, 6, 7, 4.5, 1.1, 12), (4, 3, 5, 3.2, 0.5, 10)]
ROSE_RINGS_34 = [(11.5, 8, 6, 3.5, 0.2, 12), (7, 6, 5, 3.2, 0.6, 12), (3, 4, 4, 2.6, 1.1, 10)]
SAND_64 = [
    "..........nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn..........",
    ".....nnnnnnnNNnnnnnnnnnnnnnnnNNnnnnnnnnnnnnNNnnnnnnn....",
    "..nnnNNNNNNNNmNNNNNNNNNNNNNNNNNmNNNNNNNNNNNNNmNNNNNNnnn.",
    "NNNNmNNNNNNNNNNNNNNmNNNNNNNNNNNNNNNNmNNNNNNNNNNNNNNNNNNN",
    ".NNNNNNNNmNNNNNNNNNNNNNNNmNNNNNNNNNNNNNNNNNmNNNNNNNNNNN.",
]
SAND_34 = [
    "....nnnnnnnnnnnnnnnnnnn....",
    ".nnnnNNnnnnnnnnnnNNnnnnnnn.",
    "NNNmNNNNNNNNmNNNNNNNNNmNNNN",
    ".NNNNNmNNNNNNNNNNmNNNNNNNN.",
]


def desert_rose_64():                   # the shop's header: a desert rose bedded in sand, a glint
    im = canvas(64)
    put(im, sprite(rose(64, 32, 25, ROSE_RINGS_64, 0.62, 3)), -1, -1)
    put(im, sprite(SAND_64), 3, 41)
    for x, y, ch in ((2, 53, "n"), (6, 50, "N"), (60, 50, "n"), (61, 53, "m"), (4, 56, "m"), (57, 55, "n"), (30, 52, "m"), (38, 53, "n"), (20, 52, "n")):
        grain(im, x, y, ch)
    put(im, sprite(SPARKLE, outline=False), 2, 2)
    return im


def desert_rose_34():                   # the keeper page's shop button
    im = canvas(34)
    put(im, sprite(rose(34, 17, 14, ROSE_RINGS_34, 0.62, 1.5)), -1, -1)
    put(im, sprite(SAND_34), 2, 21)
    for x, y, ch in ((2, 29, "n"), (31, 28, "N"), (5, 30, "m"), (28, 30, "n"), (17, 29, "m")):
        grain(im, x, y, ch)
    put(im, sprite(SPARKLE, outline=False), 0, 0)
    return im


def desert_rose_16():                   # the price glyph
    im = canvas(16)
    put(im, sprite(ROSE_16), 0, 0)
    put(im, sprite([".nnnnnnnnnn.", "nnNNnnnnNNnn"]), 1, 11)
    return im


# ---- the keeper page's MY TANKS: the bench and the glass kept, desert inside ----
TANK_WALL = ["qqqqqqqqqqqqq"] + ["rrrrrrrrrrrrr"] * 2 + [
    "RRRRrrrrrrrrr", "rrrRRRRrrrrrr", "rrrrrrRRRRRRR", "rrrrrrrrrrrrr", "rrrrrrrrrrrrr",
    "rrrrrrrrrRRRR", "RRRRRRRRRRrrr"] + ["rrrrrrrrrrrrr"] * 4 + [
    "nnnnnnnnnnnnn", "NNnNNNNnNNNnN", "NmNNNNmNNNNNN", "NNNNmNNNNmNNN"]
TANK_GECKO = [
    ".....HHH.",
    "..HDHYYEW",
    "HYYYDYYYY",
    ".SBBBBBBs",
    ".S.S..S.S",
]
TANK_ROCK = [                   # a rock with a pink-tipped succulent on it
    ".p.gGg.p.",
    "pGgGgGgGp",
    ".vGgGGGv.",
    "..vrrRv..",
    ".rRRRRRq.",
    "rRRqRRRqq",
]


def my_tanks():
    im = crop("foot_my_tanks", (0, 0, 36, 36))
    for x0 in (2, 21):                  # the water out, a sandstone wall and a sand floor in
        put(im, sprite(TANK_WALL, outline=False), x0, 7)
        put(im, crop("foot_my_tanks", (1, 8, 2, 20)), x0, 8)   # the glass's glint
    put(im, sprite(TANK_GECKO), 3, 15)
    put(im, sprite(TANK_ROCK), 22, 14)
    return im



ICONS_TERR = {
    "ms_first_meal": first_meal,
    "ms_first_hold_approach": first_hold_approach,
    "ms_first_seagrass": first_hide,
    "ms_first_bubbles": first_bask,
    "ms_first_follow": first_follow,
    "ms_first_dart": first_dig,
    "ms_first_feeding": first_feeding,
    "ms_first_glass_cleaning": first_glass_cleaning,
    "ms_first_quiet_night": first_quiet_night,
    "ms_first_play_session": first_play_session,
    "ms_tank_changed_someone": changed_someone,
    "ms_first_sale": first_sale,
    "icon_hunger": hunger,
    "icon_shy": shy,
    "icon_bold": bold,
    "icon_solo": solo,
    "icon_social": social,
    "icon_cautious": cautious,
    "icon_curious": curious,
    "shop_sand_dollar_16": desert_rose_16,
    "shop_sand_dollar_64": desert_rose_64,
    "foot_sand_dollar": desert_rose_34,
    "foot_my_tanks": my_tanks,
}


def main():
    for name, draw in ICONS_TERR.items():
        orig = Image.open(os.path.join(ICONS, name + ".png"))
        im = draw()
        if im.size != orig.size:
            raise SystemExit(f"{name}_terr: {im.size} but the original is {orig.size}")
        im.save(os.path.join(ICONS, name + "_terr.png"))
        print(f"  {name}_terr.png")


if __name__ == "__main__":
    main()

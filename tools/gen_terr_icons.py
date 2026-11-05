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
    # a cast skin: papery, pale
    "e": 0xf4efe4, "h": 0xd6ccb8, "i": 0xa89c84,
    # mist
    "f": 0xc4ecf6, "F": 0xeefcff,
    # sphagnum moss: pale green-yellow strands, a wet sheen
    "j": 0xdce890, "J": 0xb4cc5a, "K": 0x7e9c3a, "V": 0x4e6a28, "X": 0xf0fcff,
    # metal and plastic: the lamp's dome, the farm's tub
    "M": 0xd0d6dc, "P": 0x8c96a0, "U": 0x525a64,
    # a mealworm: yellow-brown bands, the head dark
    "y": 0xe8bc52, "a": 0xb8862c, "d": 0x7a5418,
    # the decorations, in daylight: a saguaro's greens and its spines
    "6": 0x9cc070, "7": 0x6a9850, "8": 0x47703c, "9": 0x2c4c2c, "x": 0xf2ead0,
    # bleached bone
    "I": 0xfffaf0, "Z": 0xe6dcc6, "+": 0xc2b49a, "%": 0x8c7e68, "#": 0x3e342c,
    # silvered driftwood
    "=": 0xdcd8cf, "-": 0xb2aa9e, "~": 0x857d72, ":": 0x575047,
    # terracotta
    ";": 0xec9e6c, "*": 0xc66c42, "^": 0x96482c, "&": 0x642c1c,
    # the scene's echeverias (render_scene_desert.c sd_plant): four greens, the pink tips
    "<": 0xa6cc98, ">": 0x8aba84, "[": 0x6ea06c, "]": 0x588658, "!": 0xd8889a,
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


# ---- the terrarium's own badges and shop items (no aquarium original) ----
def shed(rows, seed=0):
    """a gecko's cast skin: its outline in papery tan, the inside pale and patchy, the eye holes empty"""
    solid = {(i, j) for j, r in enumerate(rows) for i, c in enumerate(r) if c != "."}
    out = []
    for j, r in enumerate(rows):
        s = ""
        for i in range(max(len(q) for q in rows)):
            c = r[i] if i < len(r) else "."
            if c in ".EW":
                s += "."
            elif any((i + a, j + b) not in solid for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                s += "i"
            elif (i * 3 + j * 5 + seed) % 7 in (0, 3):
                s += "."                # worn through: the skin's thin
            else:
                s += "h" if c == "D" else "e"
        out.append(s)
    return out


def widen(rows, cols):
    """a character map stretched: each column in cols doubled"""
    return ["".join(c * (2 if i in cols else 1) for i, c in enumerate(r)) for r in rows]


def turtle(x, y, head, length, turn):
    """a curve from (x, y), heading `head` degrees (0 right, 90 up), bending turn(s) degrees per px: t 0..1 -> (x, y)"""
    pts, s, step = [(x, y)], 0.0, 0.25
    while s < length:
        head += turn(s) * step
        x += math.cos(math.radians(head)) * step
        y -= math.sin(math.radians(head)) * step
        s += step
        pts.append((x, y))
    return lambda t: pts[min(len(pts) - 1, int(t * (len(pts) - 1)))]


def put_worm(dst, curve, n=80):
    """a mealworm along curve, tail (t=0) to head (t=1): 2 px thick, banded, the head dark, outlined"""
    cells, length, prev, samples = {}, 0.0, None, []
    for k in range(n + 1):
        x, y = curve(k / n)
        if prev:
            length += math.hypot(x - prev[0], y - prev[1])
        prev = (x, y)
        samples.append((k / n, x, y, length))
    for t, x, y, l in samples:
        (x1, y1), (x2, y2) = curve(max(0, t - 0.01)), curve(min(1, t + 0.01))
        m = math.hypot(x2 - x1, y2 - y1) or 1
        nx, ny = -(y2 - y1) / m, (x2 - x1) / m
        c = "d" if l > length - 1.5 else "y" if int(l / 1.5) % 2 == 0 else "a"
        for px, py in ((x, y), (x + nx, y + ny)):
            cells[(round(px), round(py))] = c
    x0, y0 = min(p[0] for p in cells), min(p[1] for p in cells)
    rows = [["."] * (max(p[0] for p in cells) - x0 + 1) for _ in range(max(p[1] for p in cells) - y0 + 1)]
    for (x, y), c in cells.items():
        rows[y - y0][x - x0] = c
    put(dst, sprite(["".join(r) for r in rows]), x0 - 1, y0 - 1)


GECKO_ADULT = widen(GECKO, {6, 12, 16, 19})     # 30 x 10: the walker grown long and heavy
DOME = [                        # 22 x 12: the basking lamp's reflector, its cord, the dark lip under it
    "..........UU..........",
    "..........UU..........",
    "........MMPPPP........",
    ".....MMMMMPPPPPPP.....",
    "...MWWMMMMPPPPPPPPU...",
    "..MWMMMMMMPPPPPPPPUU..",
    ".MWMMMMMMPPPPPPPPPUUU.",
    ".MMMMMMMPPPPPPPPPPUUU.",
    "MMMMMMMMPPPPPPPPPPUUUU",
    "MMMMMMMPPPPPPPPPPUUUUU",
    "MWWWWMMMMMMMPPPPPPPPPU",
    ".UUUUqqqqqqqqqqqqUUUU.",
]
BULB = [                        # 8 x 6: the bulb, glowing
    "..llll..",
    ".lwwwlL.",
    "lwwwwlLL",
    "lwwllLLo",
    ".lLLLLo.",
    "..LLoo..",
]
TUB = [                         # 26 x 10: a plastic tub, its rim
    "WMMMMMMMMMMMMMMMMMMMMMMMMP",
    "PPPPPPPPPPPPPPPPPPPPPPPPPU",
    ".MWMMMMMMMMMMMMMMMMMMMMPU.",
    ".MWMMMMMMMMMMMMMMMMMMMMPU.",
    ".MWMMMMMMMMMMMMMMMMMMMMPU.",
    "..MWMMMMMMMMMMMMMMMMMMPU..",
    "..MWMMMMMMMMMMMMMMMMMMPU..",
    "..MWMMMMMMMMMMMMMMMMMMPU..",
    "..MMMMMMMMMMMMMMMMMMMMPU..",
    "...PPPPPPPPPPPPPPPPPPUU...",
]
BRAN = [                        # 28 x 4: the bran heaped over the rim
    "......nnnnnn.......nnnn.....",
    "..nnnnNNnnnnnnn.nnnnNNnnnn..",
    ".nNNmNNNNnNNmNNNNNNmNNNNmNn.",
    "nNNNNNNmNNNNNNNNmNNNNNNNNNNn",
]


def first_shed():                       # a clean shed: the bright gecko and its cast skin
    im = canvas()
    put(im, sprite(tilt(shed(GECKO), -0.12), outline=False), 1, 1)
    put(im, sprite(GECKO), 3, 19)
    put(im, sprite(SPARKLE, outline=False), 27, 12)
    put(im, sprite(["w"], outline=False), 24, 17)
    return im


def shed_helped():                      # a stuck shed fixed: the mister's fine spray over a gecko
    im = canvas()
    put(im, sprite(GECKO_SMALL), 1, 22)
    put(im, crop("tool_mister", (3, 2, 22, 23)), 13, 0)       # the bottle, its nozzle to the left
    drops = ((11, 3), (10, 6), (12, 7), (8, 5), (7, 9), (10, 10), (5, 8), (4, 12), (8, 13), (11, 13),
             (3, 15), (6, 16), (9, 17), (2, 19), (5, 20), (12, 16), (1, 13), (7, 19))
    for k, (x, y) in enumerate(drops):
        put(im, sprite(["F" if k % 3 == 0 else "f"], outline=False), x, y)
    return im


def clean_run():                        # three clean sheds in a row
    im = canvas()
    skin = sprite(shed(GECKO_SMALL), outline=False)
    put(im, skin, 1, 22)
    put(im, skin, 8, 13)
    put(im, skin, 15, 4)
    put(im, sprite(SPARKLE, outline=False), 3, 3)
    put(im, sprite(["w"], outline=False), 9, 6)
    return im


def first_adult():                      # grown: a big spotted adult up on its rock
    im = canvas()
    put(im, sprite(ROCK), 5, 22)
    put(im, sprite(GECKO_ADULT), 0, 12)
    return im


def moss():                             # sphagnum: a damp clump of pale strands, starry heads, a wet sheen
    w, h = 30, 17
    rows = []
    for y in range(h):
        row = ""
        for x in range(w):
            dx, dy = (x + 0.5 - 15) / 14.5, (y + 0.5 - h) / (h - 0.5)
            if dx * dx + dy * dy > 1 + 0.08 * math.sin(x * 1.3) + 0.05 * math.sin(x * 2.9 + 1):
                row += "."
                continue
            s = (x + 2.2 * math.sin(y * 0.8 + x * 0.35)) % 4      # the strands, wavering
            c = "K" if s < 1 else "j" if s < 2 else "J"
            depth = y / h + (x - 15) / 60
            if depth > 0.7:
                c = {"j": "J", "J": "K", "K": "V"}[c]
            elif depth < 0.3 and c == "J":
                c = "j"
            row += c
        rows.append(row)
    im = canvas()
    put(im, sprite(rows), 0, 12)
    for x, y in ((6, 17), (12, 14), (19, 14), (25, 17), (10, 21), (17, 19)):
        put(im, sprite([".j.", "jXj", ".j."], outline=False), x, y)
    for x, y in ((8, 16), (9, 16), (14, 15), (15, 15), (21, 15)):
        put(im, sprite(["X"], outline=False), x, y)
    for strand in (((3, 17), (2, 16), (1, 15)), ((8, 14), (7, 13), (7, 12)), ((14, 12), (14, 11), (15, 10)),
                   ((21, 12), (22, 11), (22, 10)), ((27, 15), (28, 14), (29, 13))):
        for x, y in strand:
            put(im, sprite(["J"], outline=False), x, y)
    for x, y in ((1, 29), (30, 28)):    # drips
        put(im, sprite(["f"]), x, y)
    return im


def bulb():                             # a basking bulb: the dome reflector, the bulb lit, warm rays
    im = canvas()
    for ray in (((15, 23), (15, 31)), ((16, 23), (16, 31)), ((12, 22), (8, 30)), ((19, 22), (23, 30)),
                ((10, 20), (4, 26)), ((21, 20), (27, 26))):
        line(im, list(ray), "L")
    for ray in (((13, 26), (11, 31)), ((18, 26), (20, 31)), ((8, 19), (3, 21)), ((23, 19), (28, 21))):
        line(im, list(ray), "o")
    put(im, sprite(BULB), 11, 14)
    put(im, sprite(DOME), 4, 1)
    return im


def farm():                             # a mealworm farm: a tub of bran, the worms curling out
    im = canvas()
    put_worm(im, turtle(6, 14, 95, 15, lambda s: 0 if s < 6 else 21))
    put_worm(im, turtle(24, 14, 85, 14, lambda s: 0 if s < 5 else -24))
    put_worm(im, turtle(15, 14, 90, 9, lambda s: 0 if s < 4 else 30))
    put(im, sprite(BRAN), 1, 12)
    put(im, sprite(TUB), 2, 17)
    return im


# ---- the desert decorations: a shop icon (32 px, outlined) and the piece itself as it stands on the sand
# (no outline, no ground, no shadow: the scene draws the contact shadow and lights it; its foot is the bottom row's centre)
def grid(w, h):
    return [["."] * w for _ in range(h)]


def joined(g):
    return ["".join(r) for r in g]


def stem(g, path, r, tones, spines=0, light=(-0.75, -0.65), seed=0):
    """a ribbed round stem along a polyline, half-width r: tones lit, mid, shade, dark; ribs run along it, spines on them"""
    lx, ly = light
    for y in range(len(g)):
        for x in range(len(g[0])):
            px, py, best = x + 0.5, y + 0.5, None
            for (x0, y0), (x1, y1) in zip(path, path[1:]):
                dx, dy = x1 - x0, y1 - y0
                t = max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)))
                qx, qy = x0 + t * dx, y0 + t * dy
                d = math.hypot(px - qx, py - qy)
                if best is None or d < best[0]:
                    n = math.hypot(dx, dy)
                    nx, ny = -dy / n, dx / n
                    if nx * lx + ny * ly < 0:
                        nx, ny = -nx, -ny               # n: toward the light
                    best = (d, ((px - qx) * nx + (py - qy) * ny) / r)
            d, s = best
            if d > r:
                continue
            k = int((1 - s) * r)                        # which rib
            c = tones[0] if s > 0.35 else tones[1] if s > -0.4 else tones[2]
            if k % 2:
                c = {tones[0]: tones[1], tones[1]: tones[2], tones[2]: tones[3]}[c]
            if s < -0.82:
                c = tones[3]
            if spines and k % 2 == 0 and c != tones[3] and (y * 2 + k * 3 + seed) % spines == 0 and d < r - 0.6:
                c = "x"
            g[y][x] = c


def saguaro(w, h, trunk_r, arm_r, cx, top, arms, spines=7):
    g = grid(w, h)
    for path in arms:
        stem(g, path, arm_r, "6789", spines, seed=3)
    stem(g, [(cx, top), (cx, h + trunk_r)], trunk_r, "6789", spines)
    return joined(g)


def wood(g, path, radii, twist, light=(-0.45, -0.9), period=4.0, width=0.85):
    """a silvered branch along path, radii[i] thick at each point; the grain twists round it"""
    lx, ly = light
    for y in range(len(g)):
        for x in range(len(g[0])):
            px, py, best = x + 0.5, y + 0.5, None
            for i, ((x0, y0), (x1, y1)) in enumerate(zip(path, path[1:])):
                dx, dy = x1 - x0, y1 - y0
                t = max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)))
                qx, qy = x0 + t * dx, y0 + t * dy
                r = radii[i] + (radii[i + 1] - radii[i]) * t
                d = math.hypot(px - qx, py - qy) / r
                if best is None or d < best[0]:
                    n = math.hypot(dx, dy)
                    nx, ny = -dy / n, dx / n
                    if nx * lx + ny * ly < 0:
                        nx, ny = -nx, -ny
                    best = (d, ((px - qx) * nx + (py - qy) * ny) / r, r, (t + i) / (len(path) - 1))
            d, s, r, along = best
            if d > 1:
                continue
            c = "=" if s > 0.3 else "-" if s > -0.45 else "~"
            if s < -0.8:
                c = ":"
            if (s * r + along * len(path) * twist * 6) % period < width and c != ":":
                c = {"=": "-", "-": "~", "~": ":"}[c]
            g[y][x] = c


def driftwood(w, h, parts):
    g = grid(w, h)
    for path, radii, twist in parts:
        wood(g, path, radii, twist)
    return joined(g)


ECHE_SIN = (0.0, -0.5227, 0.5227, -0.8674, 0.8674, -0.9927, 0.9927)
ECHE_COS = (1.0, 0.8525, 0.8525, 0.4976, 0.4976, 0.1205, 0.1205)
ECHE_LEN = (12, 11.5, 11.5, 10.5, 10.5, 9, 9)


def echeveria(g, cx, cy, sc):
    """the scene's echeveria (render_scene_desert.c sd_plant) scaled by sc: pointed leaves fanning up, pink tips"""
    darker, lighter = {"<": ">", ">": "[", "[": "]", "]": "]"}, {"<": "<", ">": "<", "[": ">", "]": "["}
    for y in range(len(g)):
        for x in range(len(g[0])):
            dx, dy = (x + 0.5 - cx) / sc, (y + 0.5 - cy) / sc
            if dx < -13 or dx > 13 or dy < -13 or dy > 2:
                continue
            for k in range(6, -1, -1):                  # the outer, lower leaves are the nearer
                u, v = dx * ECHE_SIN[k] - dy * ECHE_COS[k], dx * ECHE_COS[k] + dy * ECHE_SIN[k]
                if u < 0 or u > ECHE_LEN[k]:
                    continue
                q = u / ECHE_LEN[k]
                wd = 3 * (q / 0.35 if q < 0.35 else (1 - q) / 0.65) + 0.4
                if abs(v) > wd:
                    continue
                c = "<" if k < 1 else ">" if k < 3 else "[" if k < 5 else "]"
                if q > 0.84:
                    c = "!"
                elif v > wd - 0.9 / sc:
                    c = darker[c]
                elif v < -wd + 0.9 / sc:
                    c = lighter[c]
                g[y][x] = c
                break


def pot(w, h, top, rim_w, body_top_w, body_bot_w, plants):
    """a terracotta pot, its rim from row top, tapering to its foot on the bottom row; plants (x, y, scale) in it"""
    g = grid(w, h)
    for px, py, sc in plants:
        echeveria(g, px, py, sc)
    for y in range(top, h):
        t = (y - top - 3) / max(1, h - 1 - top - 3)
        half = rim_w / 2 if y < top + 3 else (body_top_w + (body_bot_w - body_top_w) * t) / 2
        for x in range(w):
            u = (x + 0.5 - w / 2) / half
            if abs(u) > 1:
                continue
            c = "&" if u > 0.85 else ";" if u < -0.45 else "*" if u < 0.4 else "^"
            if y == top:
                c = {";": ";", "*": ";", "^": "*", "&": "^"}[c]
            elif y == top + 2:
                c = {";": "*", "*": "^", "^": "^", "&": "&"}[c]
            elif y == top + 3:                          # the rim's shadow on the body
                c = {";": "^", "*": "^", "^": "&", "&": "&"}[c]
            g[y][x] = c
    return joined(g)


SKULL_LEFT = [                  # 16 x 18: a cow skull's left half, the horn sweeping out and up
    ".+..............",
    ".Z%.............",
    ".ZZ%............",
    "..ZZ%......ZZZZZ",
    "...ZZZ++..ZIIIII",
    "....ZZZZZZZIIIII",
    ".....%%%%%ZIIZII",
    ".........ZIIIIII",
    ".........Z##+ZII",
    ".........Z###ZII",
    "..........Z##ZII",
    "..........ZZIIII",
    "...........ZIIII",
    "...........ZIZ#I",
    "............ZZ#I",
    "............ZZ+I",
    "............ZZZI",
    ".............+ZZ",
]


def skull():
    """the whole skull: the left half mirrored a shade darker, a dark edge down its shaded side and under the jaw"""
    darker = {"I": "Z", "Z": "+", "+": "%", "%": "%", "#": "#"}
    g = [list(r + "".join(darker.get(c, c) for c in reversed(r))) for r in SKULL_LEFT]
    h, w = len(g), len(g[0])
    for y in range(h):
        for x in range(w // 2, w):
            if g[y][x] in "IZ+" and (x + 1 == w or g[y][x + 1] == "."):
                g[y][x] = "%"
    g[h - 1] = [{"I": "Z", "Z": "+", "+": "%"}.get(c, c) for c in g[h - 1]]
    return joined(g)


SAGUARO_ARMS = [[(11, 31), (5.5, 31), (3.5, 28), (3.5, 13)], [(11, 24), (16.5, 24), (18.5, 21), (18.5, 8)]]
DRIFTWOOD = [                   # (path, radii, twist): a root snag, a branch stub reaching up, the trunk lying
    ([(8, 9.5), (4.5, 4.5), (3, 1.5)], [2.0, 1.5, 1.0], 0.5),
    ([(31, 10), (38, 5.5), (45, 2.4), (50, 1.6)], [2.0, 1.6, 1.2, 0.9], 0.5),
    ([(5.5, 10.5), (16, 11.4), (31, 12.4), (43, 11.3), (54, 9.8)], [4.3, 3.9, 3.3, 2.5, 1.4], 0.6),
]
DRIFTWOOD_ICON = [
    ([(6, 13), (3, 6), (2.5, 2)], [2.2, 1.6, 1.0], 0.5),
    ([(17, 13), (22, 7), (25, 3), (28, 1.8)], [2.0, 1.6, 1.2, 0.9], 0.5),
    ([(4.5, 14), (11, 15.5), (18, 16.3), (25, 15.2), (29, 13.6)], [4.2, 3.8, 3.3, 2.5, 1.3], 0.6),
]
SAND_ICON = [
    "......nnnnnnnnnnnnnnnnnnn......",
    "..nnnnNNnnnnnnnnnnnnnNNnnnnnn..",
    "nNNNNNNNNmNNNNNNNNNNNNNmNNNNNNN",
]


def piece(rows):
    return sprite(rows, outline=False)


def deco_saguaro():                     # 22 x 48
    return piece(saguaro(22, 48, 4.2, 2.7, 11, 4.5, SAGUARO_ARMS))


def deco_skull():                       # 32 x 18
    return piece(skull())


def deco_driftwood():                   # 56 x 16
    return piece(driftwood(56, 16, DRIFTWOOD))


def deco_pot():                         # 24 x 28
    return piece(pot(24, 28, 16, 22, 19, 14, [(5.5, 17, 0.62), (18.5, 17, 0.62), (12, 17.5, 0.98)]))


def shop_saguaro():
    im = canvas()
    arms = [[(13, 19), (7.2, 19), (5.2, 16.5), (5.2, 7)], [(13, 14), (18.8, 14), (20.8, 11.5), (20.8, 4)]]
    put(im, sprite(saguaro(26, 29, 3.4, 2.2, 13, 3.6, arms, spines=5)), 2, 0)
    put(im, sprite([r[4:26] for r in SAND_ICON]), 4, 27)
    return im


def shop_skull():
    im = canvas()
    put(im, sprite([r[4:26] for r in SAND_ICON]), 4, 25)
    put(im, sprite([r[1:31] for r in skull()]), 0, 8)
    return im


def shop_driftwood():
    im = canvas()
    put(im, sprite(SAND_ICON), 0, 26)
    put(im, sprite(driftwood(30, 21, DRIFTWOOD_ICON)), 0, 7)
    return im


def shop_pot():
    im = canvas()
    put(im, sprite(pot(26, 30, 17, 24, 21, 15, [(6, 18, 0.66), (20, 18, 0.66), (13, 18.5, 1.05)])), 2, 0)
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

# the terrarium's own: no aquarium original to size against, so each carries its size
ICONS_TERR_ONLY = {
    "ms_first_shed": (first_shed, (32, 32)),
    "ms_shed_helped": (shed_helped, (32, 32)),
    "ms_clean_run": (clean_run, (32, 32)),
    "ms_first_adult": (first_adult, (32, 32)),
    "shop_moss": (moss, (32, 32)),
    "shop_bulb": (bulb, (32, 32)),
    "shop_farm": (farm, (32, 32)),
    "shop_saguaro": (shop_saguaro, (32, 32)),
    "shop_skull": (shop_skull, (32, 32)),
    "shop_driftwood": (shop_driftwood, (32, 32)),
    "shop_pot": (shop_pot, (32, 32)),
    "deco_saguaro": (deco_saguaro, (22, 48)),
    "deco_skull": (deco_skull, (32, 18)),
    "deco_driftwood": (deco_driftwood, (56, 16)),
    "deco_pot": (deco_pot, (24, 28)),
}


def main():
    for name, draw in ICONS_TERR.items():
        orig = Image.open(os.path.join(ICONS, name + ".png"))
        im = draw()
        if im.size != orig.size:
            raise SystemExit(f"{name}_terr: {im.size} but the original is {orig.size}")
        im.save(os.path.join(ICONS, name + "_terr.png"))
        print(f"  {name}_terr.png")
    for name, (draw, size) in ICONS_TERR_ONLY.items():
        im = draw()
        if im.size != size:
            raise SystemExit(f"{name}_terr: {im.size} but it should be {size[0]}x{size[1]}")
        im.save(os.path.join(ICONS, name + "_terr.png"))
        print(f"  {name}_terr.png")


if __name__ == "__main__":
    main()

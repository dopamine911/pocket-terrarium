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

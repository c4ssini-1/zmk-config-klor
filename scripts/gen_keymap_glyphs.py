#!/usr/bin/env python3
"""
Generate src/keymap_glyphs.h from config/klor.keymap.

The OLED draws a miniature keymap of the active layer. That needs one glyph per
key per layer, which is 4 x 44 entries -- far too much to maintain by hand
without it drifting away from the real keymap. So it is derived from the keymap
instead, and this script is the only thing that should ever write that header.

Run after any keymap change:

    python3 scripts/gen_keymap_glyphs.py

Two things it does that are not obvious:

* `&trans` is resolved, not printed as a blank. The layers chain deterministically
  (BASE < XTRA < FN < SYS, each only reachable through the one before), so a
  transparent key always falls through to a known binding and the glyph shown is
  what you will actually get.

* The grid mirrors the physical matrix. Each position's (row, column) comes from
  the transform in klor.dtsi, so the picture matches the board rather than the
  order bindings happen to be listed in.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYMAP = ROOT / "config/klor.keymap"
DTSI = ROOT / "boards/shields/klor/klor.dtsi"
OUT = ROOT / "src/keymap_glyphs.h"

# One character per key. unscii_8 is plain ASCII, so no clever symbols.
# Ambiguity is tolerated where the alternative is showing nothing useful:
# lowercase marks a named key, so 'e' is Escape while 'E' is the letter.
GLYPH = {
    "EXCL": "!", "AT": "@", "HASH": "#", "DLLR": "$", "PRCNT": "%",
    "CARET": "^", "AMPS": "&", "ASTRK": "*", "LPAR": "(", "RPAR": ")",
    "MINUS": "-", "UNDER": "_", "EQUAL": "=", "PLUS": "+",
    "LBKT": "[", "RBKT": "]", "LBRC": "{", "RBRC": "}",
    "BSLH": "\\", "PIPE": "|", "SEMI": ";", "COLON": ":",
    "SQT": "'", "DQT": '"', "COMMA": ",", "DOT": ".", "FSLH": "/",
    "GRAVE": "`", "TILDE": "~",
    "SPACE": " ", "RET": "r", "BSPC": "<", "TAB": ">", "ESC": "e", "DEL": "d",
    "LSHFT": "s", "RSHFT": "s", "LCTRL": "c", "RCTRL": "c",
    "LALT": "a", "RALT": "a", "LGUI": "g", "RGUI": "g",
    "C_MUTE": "m", "C_PP": "p", "C_VOL_UP": "+", "C_VOL_DN": "-",
    "C_NEXT": ">", "C_PREV": "<", "C_BRI_UP": "+", "C_BRI_DN": "-",
    "CAPS": "k", "K_APP": "n", "PSCRN": "r", "INS": "i",
    "HOME": "h", "END": "n", "PG_UP": "u", "PG_DN": "j",
    "UP": "^", "DOWN": "v", "LEFT": "<", "RIGHT": ">",
}


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def layer_bindings(clean):
    """Ordered {node_name: [44 bindings]} -- order is the layer index."""
    out = {}
    for name, body in re.findall(r"(\w+_layer)\s*\{(.*?)\n        \}", clean, re.S):
        m = re.search(r"bindings\s*=\s*<(.*?)>;", body, re.S)
        if not m:
            continue
        toks = [" ".join(t.split())
                for t in re.findall(r"&\S+(?:\s+(?!&)\S+)*", m.group(1))]
        out[name] = toks
    return out


def grid_positions(dtsi):
    """position index -> (row, col), straight from the matrix transform."""
    blk = re.search(r"map = <(.*?)>;", dtsi, re.S).group(1)
    return {i: (int(r), int(c))
            for i, (r, c) in enumerate(re.findall(r"RC\((\d+),(\d+)\)", blk))}


def glyph_for(binding):
    m = re.fullmatch(r"&kp (\w+)", binding)
    if m:
        k = m.group(1)
        if k in GLYPH:
            return GLYPH[k]
        if re.fullmatch(r"N\d", k):        # N1..N0 -> the digit
            return k[1]
        if re.fullmatch(r"F\d+", k):       # F1..F12 -> 1..9, then a b c
            n = int(k[1:])
            return str(n) if n <= 9 else "abc"[n - 10]
        if len(k) == 1:
            return k
        return k[0]
    if binding.startswith("&mo "):
        return binding.split()[-1][0].lower()   # xtra -> x, fn -> f, sys -> s
    if binding.startswith("&spc"):
        return " "                              # tap is Space, drawn blank
    if binding.startswith("&bt "):
        return "b"
    if binding.startswith("&rgb"):      # &rgb (ours) and ZMK's old &rgb_ug
        return "*"
    if binding.startswith("&out"):
        return "o"
    if binding == "&bootloader":
        return "!"
    if binding == "&sys_reset":
        return "R"
    if binding.startswith("&ext_power"):
        return "P"
    if binding == "&none":
        return " "
    return "?"


def main():
    clean = strip_comments(KEYMAP.read_text())
    layers = layer_bindings(clean)
    pos = grid_positions(DTSI.read_text())
    npos = len(pos)

    names = list(layers)
    if len(names) != len({n for n in names}):
        sys.exit("duplicate layer nodes")

    # Layers chain, so a &trans on layer n resolves downwards through n-1..0.
    resolved = {}
    for idx, name in enumerate(names):
        row = []
        for p in range(npos):
            b = layers[name][p]
            i = idx
            while b == "&trans" and i > 0:
                i -= 1
                b = layers[names[i]][p]
            row.append(b)
        resolved[name] = row

    ncols = max(c for _, c in pos.values()) + 1
    nrows = max(r for r, _ in pos.values()) + 1

    lines = [
        "/*",
        " * GENERATED by scripts/gen_keymap_glyphs.py -- do not edit by hand.",
        " * Regenerate after any change to config/klor.keymap.",
        " *",
        " * One character per key, laid out on the physical matrix grid, for each",
        " * layer. &trans is already resolved down the layer chain, so what is shown",
        " * is what the key actually does.",
        " */",
        "",
        "#pragma once",
        "",
        f"#define KEYMAP_GLYPH_ROWS {nrows}",
        f"#define KEYMAP_GLYPH_COLS {ncols}",
        f"#define KEYMAP_GLYPH_LAYERS {len(names)}",
        "",
        "/* [layer][row] -> one string of KEYMAP_GLYPH_COLS chars, ' ' where no key. */",
        f"static const char *const keymap_glyphs[{len(names)}][{nrows}] = {{",
    ]

    for name in names:
        grid = [[" "] * ncols for _ in range(nrows)]
        for p in range(npos):
            r, c = pos[p]
            grid[r][c] = glyph_for(resolved[name][p])
        lines.append(f"    {{ /* {name.split('_')[0].upper()} */")
        for r in range(nrows):
            body = "".join(grid[r]).replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'        "{body}",')
        lines.append("    },")

    lines += ["};", ""]

    # The screen also needs the inverse lookup: given a key position from a
    # zmk_position_state_changed event, which cell of the grid above does it
    # light up? Emitted from the same `pos` map, so the picture and the
    # highlight can never disagree about where a key lives.
    lines += [
        f"#define KEYMAP_GLYPH_POSITIONS {npos}",
        "",
        "/* Key position -> cell in the grid above. */",
        f"static const unsigned char keymap_pos_row[{npos}] = {{",
        "    " + ", ".join(str(pos[p][0]) for p in range(npos)),
        "};",
        f"static const unsigned char keymap_pos_col[{npos}] = {{",
        "    " + ", ".join(str(pos[p][1]) for p in range(npos)),
        "};",
        "",
    ]

    OUT.write_text("\n".join(lines))

    print(f"wrote {OUT.relative_to(ROOT)}  ({len(names)} layers, {nrows}x{ncols})")
    for name in names:
        print(f"  --- {name}")
        grid = [[" "] * ncols for _ in range(nrows)]
        for p in range(npos):
            r, c = pos[p]
            grid[r][c] = glyph_for(resolved[name][p])
        for r in range(nrows):
            print("      |" + "".join(grid[r]) + "|")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check the firmware's colour generator against the companion's.

Colour used to be a table: the app worked out 93 bytes per board and the board
just held them, so there was nothing to disagree about. Now the board generates
its own colours from the 40 bytes of the `K` frame -- which is what lets a board
hot-plugged into a grid colour itself with no host connected -- and that puts a
second implementation of "what colour is this key" on the other side of a wire.

  1. Extracts `colorgen_params_t`, `floor_div`, `mod_pos`, `round_pct`,
     `colorgen_for_xy` and `colorgen_params_apply` VERBATIM from Core/Src/main.c,
     so what runs here is the firmware's code rather than a paraphrase of it.
  2. Compiles them with clang and feeds them real `K` frames built by the
     companion's own `colorgenFrame`, over both layouts, palettes of one to eight
     colours, four brightnesses, the C/D/E tint on and off, two placement offsets
     and a four-board grid.
  3. Diffs the result against `generateColorAt` -- the companion's actual
     authoring path, which asks meantonal for the accidental. Unlike the pitch
     check, no JS mirror of the integer arithmetic is needed: the reference here
     is the real implementation, so agreement means the two genuinely agree and
     not that two copies of one formula match.
  4. Separately asserts the headline compatibility claim: at the compiled-in
     defaults the generator is byte-identical to the hardcoded fill_bosanquet()
     it replaced, so flashing this firmware cannot change how a board looks until
     something pushes a `K` frame.

Run with `npm run check:colors`. Needs clang and node; it writes only into a
temporary directory.
"""

from __future__ import annotations

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
COMPANION = HERE.parent
MAIN_C = COMPANION.parent / "Core" / "Src" / "main.c"

BOARDS = 4

# The five accidental rows of the pattern this replaced, as fill_bosanquet() laid
# them out by LED index -- fill_bosanquet(5,5,25, 5,25,5, 20,20,2, 25,10,2, 25,2,2)
# in main.c, whose arguments run double-sharp, sharp, natural, flat, double-flat.
LEGACY_GROUPS = [
    ((25, 2, 2), [0, 5, 6, 18, 19]),           # double-flat
    ((25, 10, 2), [1, 3, 4, 7, 16, 17, 20]),   # flat
    ((20, 20, 2), [2, 8, 9, 15, 21, 22, 28]),  # natural
    ((5, 25, 5), [10, 13, 14, 23, 26, 27, 29]),  # sharp
    ((5, 5, 25), [11, 12, 24, 25, 30]),        # double-sharp
]

# Distinct, deliberately awkward values: channels that round differently at each
# brightness, and a few at the extremes.
PALETTE_POOL = [
    "#fa1414", "#fa6414", "#c8c814", "#32fa32",
    "#3232fa", "#7f7f7f", "#010203", "#fffefd",
]

CONFIG_AXES = {
    "layout": ["bosanquet", "wicki-hayden"],
    "palette_n": [1, 2, 3, 5, 8],
    "brightness": [1, 10, 60, 100],
    "lightenCDE": [False, True],
    "offset": [[0, 0], [3, -1]],
    "startAccidental": [-2, 0],
}


def configs() -> list[dict]:
    """Every combination of the axes above, in a fixed order both sides follow."""
    out: list[dict] = []
    for layout in CONFIG_AXES["layout"]:
        for n in CONFIG_AXES["palette_n"]:
            for brightness in CONFIG_AXES["brightness"]:
                for lighten in CONFIG_AXES["lightenCDE"]:
                    for offset in CONFIG_AXES["offset"]:
                        for start in CONFIG_AXES["startAccidental"]:
                            out.append({
                                "layout": layout,
                                "palette": PALETTE_POOL[:n],
                                "startAccidental": start,
                                "offset": offset,
                                "brightness": brightness,
                                "lightenCDE": lighten,
                            })
    return out


def grab(src: str, sig: str, frm: int = 0) -> str:
    """The brace-balanced declaration starting at `sig`.

    Lifted from check-firmware-pitch.py, and for the same reason: a function ends
    at its closing brace, while a typedef or an initialiser carries on to the
    semicolon after it (`} colorgen_params_t;`).
    """
    i = src.index(sig, frm)
    k = src.index("{", i)
    depth = 0
    while True:
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    needs_semicolon = src[i:k].lstrip().startswith(("typedef", "static volatile", "static const"))
    return src[i : (src.index(";", k) + 1) if needs_semicolon else (k + 1)]


def build_harness(src: str, led_pos: list[list[int]]) -> str:
    defines = "\n".join(
        l for l in src.splitlines() if re.match(r"#define (COLORGEN_|TUNE_MATRIX|TUNE_ANCHOR|TUNE_W0|TUNE_H0)", l)
    )
    params = grab(src, "typedef struct {", src.index("int8_t  start_acc") - 400)
    if "start_acc" not in params:
        sys.exit("could not locate colorgen_params_t in main.c")
    decl = grab(src, "static volatile colorgen_params_t colorgen =")
    floor_div = grab(src, "static int32_t floor_div(int32_t num, int32_t den)")
    mod_pos = grab(src, "static int32_t mod_pos(int32_t v, int32_t m)")
    round_pct = grab(src, "static uint8_t round_pct(uint32_t num, uint32_t den)")
    gen = grab(src, "static void colorgen_for_xy(int32_t x, int32_t y, uint8_t *rgb)")
    apply_fn = grab(src, "static uint8_t colorgen_params_apply(const uint8_t *m)")

    pos = ",".join(f"{{{x},{y}}}" for x, y in led_pos)
    legacy = ",".join(
        "{%d,%d,%d}" % next(c for c, leds in LEGACY_GROUPS if led in leds)
        for led in range(len(led_pos))
    )

    return f"""/* GENERATED by check-firmware-colors.py from Core/Src/main.c. Do not edit. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
{defines}
{params}
{decl}
{floor_div}
{mod_pos}
{round_pct}
{gen}
{apply_fn}

static const int LED_POS[{len(led_pos)}][2] = {{ {pos} }};

/* What fill_bosanquet() painted, by LED index. The default parameters must
 * reproduce this exactly or flashing changes how a board looks. */
static const uint8_t LEGACY[{len(led_pos)}][3] = {{ {legacy} }};

#define NKEYS  ((int)(sizeof(LED_POS)/sizeof(LED_POS[0])))
#define NBOARD {BOARDS}

int main(void)
{{
  /* 1. the compiled-in default must be byte-identical to the old boot pattern */
  int bad = 0;
  for (int k = 0; k < NKEYS; k++) {{
    uint8_t rgb[3];
    colorgen_for_xy(LED_POS[k][0], LED_POS[k][1], rgb);
    if (memcmp(rgb, LEGACY[k], 3) != 0) {{
      bad++;
      fprintf(stderr, "default differs at led %d (%d,%d): %u,%u,%u vs %u,%u,%u\\n",
              k, LED_POS[k][0], LED_POS[k][1],
              rgb[0], rgb[1], rgb[2], LEGACY[k][0], LEGACY[k][1], LEGACY[k][2]);
    }}
  }}
  printf("DEFAULT_IDENTICAL %d %d\\n", bad == 0, NKEYS);

  /* 2. validation must refuse what the companion would never send */
  uint8_t m[COLORGEN_MSG_LEN];
  memset(m, 0, sizeof(m));
  #define PUT16(i, v) do {{ m[i] = (uint8_t)(v); m[(i)+1] = (uint8_t)((v) >> 8); }} while (0)
  #define RESET() do {{ PUT16(0,1); PUT16(2,0); PUT16(4,0); PUT16(6,1); \\
                        PUT16(8,23); PUT16(10,7); m[12]=(uint8_t)(-2); m[13]=5; \\
                        m[14]=0; m[15]=10; }} while (0)
  RESET(); m[13] = 0;               printf("REJECT palette_empty %d\\n", !colorgen_params_apply(m));
  RESET(); m[13] = COLORGEN_PAL_MAX + 1;
                                    printf("REJECT palette_long %d\\n",  !colorgen_params_apply(m));
  RESET(); m[15] = 0;               printf("REJECT bright_zero %d\\n",   !colorgen_params_apply(m));
  RESET(); m[15] = 101;             printf("REJECT bright_over %d\\n",   !colorgen_params_apply(m));
  RESET(); PUT16(2, 1); PUT16(4, 1);
                                    printf("REJECT singular %d\\n",      !colorgen_params_apply(m));
  RESET(); PUT16(0, 999);           printf("REJECT matrix %d\\n",        !colorgen_params_apply(m));
  RESET(); PUT16(8, 9999);          printf("REJECT anchor %d\\n",        !colorgen_params_apply(m));
  RESET();                          printf("REJECT none %d\\n",          colorgen_params_apply(m));

  /* 3. every frame on stdin, one 40-byte payload per line as hex, applied and
   *    evaluated over a four-board grid for diffing against the companion. */
  char hex[4096];
  int cfg = 0;
  while (fgets(hex, sizeof(hex), stdin)) {{
    if (hex[0] == '\\n' || hex[0] == '\\0') continue;
    for (int i = 0; i < COLORGEN_MSG_LEN; i++) {{
      unsigned byte = 0;
      sscanf(hex + i * 2, "%2x", &byte);
      m[i] = (uint8_t)byte;
    }}
    if (!colorgen_params_apply(m)) {{
      printf("ROW %d rejected\\n", cfg);
    }} else {{
      for (int b = 0; b < NBOARD; b++) for (int k = 0; k < NKEYS; k++) {{
        int32_t x = LED_POS[k][0] + 5 * b, y = LED_POS[k][1] + 2 * b;
        uint8_t rgb[3];
        colorgen_for_xy(x, y, rgb);
        printf("ROW %d %ld %ld %u %u %u\\n",
               cfg, (long)x, (long)y, rgb[0], rgb[1], rgb[2]);
      }}
    }}
    cfg++;
  }}
  return 0;
}}
"""


# The reference side. Note what this does NOT contain: any copy of the firmware's
# integer arithmetic. It calls the companion's real generator and its real frame
# builder, so a pass means the two implementations agree rather than that two
# transcriptions of one formula match.
REFERENCE_MJS = """
// Absolute file URLs, because this module runs from a temporary directory: a
// relative import would resolve against that, not against the companion.
const { readFileSync } = await import("node:fs");
const { pathToFileURL } = await import("node:url");
const lib = (f) => pathToFileURL(`${process.argv[3]}/src/lib/${f}`).href;

const { colorgenFrame, generateColorAt } = await import(lib("colorMaps.ts"));
const { LED_POS } = await import(lib("layout.ts"));
const { configs, boards } = JSON.parse(readFileSync(process.argv[2], "utf8"));

const hex2 = (n) => n.toString(16).padStart(2, "0");
const out = [];
configs.forEach((cfg, i) => {
  const frame = colorgenFrame(cfg);
  if (!frame) {
    out.push(`FRAME ${i} -`);
    out.push(`ROW ${i} rejected`);
    return;
  }
  // Byte 0 is 'K'; the harness wants the payload only.
  out.push(`FRAME ${i} ` + [...frame.slice(1)].map(hex2).join(""));
  for (let b = 0; b < boards; b++) {
    for (const [lx, ly] of LED_POS) {
      const x = lx + 5 * b, y = ly + 2 * b;
      const hex = generateColorAt(cfg, x, y);
      const v = parseInt(hex.slice(1), 16);
      out.push(`ROW ${i} ${x} ${y} ${(v >> 16) & 0xff} ${(v >> 8) & 0xff} ${v & 0xff}`);
    }
  }
});
console.log(out.join("\\n"));
"""


def main() -> int:
    if not shutil.which("clang"):
        sys.exit("clang not found")
    src = MAIN_C.read_text()

    cfgs = configs()

    # LED_POS comes from the app, so the two sides cannot disagree about which
    # coordinates exist.
    led_pos = json.loads(
        subprocess.run(
            [
                "node",
                "--import",
                str(HERE / "ts-resolve.mjs"),
                "-e",
                'const {LED_POS} = await import("./src/lib/layout.ts");'
                "console.log(JSON.stringify(LED_POS));",
            ],
            cwd=COMPANION,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    )

    with tempfile.TemporaryDirectory() as tmp:
        tmpd = pathlib.Path(tmp)

        (tmpd / "reference.mjs").write_text(REFERENCE_MJS)
        (tmpd / "payload.json").write_text(json.dumps({"configs": cfgs, "boards": BOARDS}))
        ref = subprocess.run(
            [
                "node",
                "--import",
                str(HERE / "ts-resolve.mjs"),
                str(tmpd / "reference.mjs"),
                str(tmpd / "payload.json"),
                str(COMPANION),
            ],
            cwd=COMPANION,
            capture_output=True,
            text=True,
            check=True,
        ).stdout

        frames = [l.split()[2] for l in ref.splitlines() if l.startswith("FRAME")]
        if len(frames) != len(cfgs):
            sys.exit(f"reference built {len(frames)} frames for {len(cfgs)} configs")
        if any(f == "-" for f in frames):
            sys.exit("the companion refused to build a frame for a config it should accept")

        (tmpd / "harness.c").write_text(build_harness(src, led_pos))
        subprocess.run(
            ["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(tmpd / "harness"),
             str(tmpd / "harness.c")],
            check=True,
        )
        c_out = subprocess.run(
            [str(tmpd / "harness")],
            input="\n".join(frames) + "\n",
            capture_output=True,
            text=True,
            check=True,
        )

    failures = 0
    checked = 0

    for line in c_out.stdout.splitlines():
        if line.startswith("DEFAULT_IDENTICAL"):
            _, identical, n = line.split()
            if identical == "1":
                print(f"  the default generator is byte-identical to the old boot pattern ({n} keys)")
            else:
                failures += 1
                print("  FAIL  the default is NOT byte-identical to the old boot pattern")
        elif line.startswith("REJECT"):
            _, what, rejected = line.split()
            checked += 1
            if rejected != "1":
                failures += 1
                if what == "none":
                    print("  FAIL  a valid frame was rejected")
                else:
                    print(f"  FAIL  a {what} frame was accepted")

    print(f"  validation accepts and refuses all {checked} frames as intended")

    c_rows = [l for l in c_out.stdout.splitlines() if l.startswith("ROW")]
    ref_rows = [l for l in ref.splitlines() if l.startswith("ROW")]
    if c_rows == ref_rows:
        print(
            f"  firmware colours match the companion's on all {len(c_rows)} cases"
            f" ({len(cfgs)} generators x {len(led_pos) * BOARDS} keys)"
        )
    else:
        failures += 1
        n_diff = sum(a != b for a, b in zip(c_rows, ref_rows))
        print(f"  FAIL  {n_diff} of {len(c_rows)} differ")
        shown = 0
        for a, b in zip(c_rows, ref_rows):
            if a != b:
                idx = int(a.split()[1])
                print(f"          config:    {json.dumps(cfgs[idx])}")
                print(f"          firmware:  {a}")
                print(f"          companion: {b}")
                shown += 1
                if shown == 3:
                    break

    print("PASS" if failures == 0 else "FAIL")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

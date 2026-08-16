# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A **ZMK user config that is also a Zephyr module.** It holds a keymap, a Kconfig fragment,
a vendored copy of the `klor` shield, and — unusually for a user config — **C source of its
own** in [src/](src/). ZMK itself is fetched at build time by `west` per
[config/west.yml](config/west.yml); nothing here compiles standalone.

Forked from `GEIGEIGEIST/zmk-config-klor` and repaired for current ZMK. Targets a
**polydactyl KLOR (44 positions), BLE, two nice!nano v2 controllers, OLEDs on both halves**.
Hardware design files live in the separate `GEIGEIGEIST/KLOR` repo, whose
`PCB/klor1_3/klor1_3.kicad_sch` and `docs/images/KLOR*.png` are the authority on pinout and
LED order.

**Current hardware state:** the left controller has a broken **P0.09** pin (`pro_micro 10`),
which is matrix column 5 on that half. Positions 4, 15, 27 (`T`, `G`, `B`) and 28 (left
encoder push) are therefore dead, along with the four LEDs under them. Not a firmware bug —
do not try to fix it in config. The right controller is intact.

## Build and flash

There is **no local build loop** — no `west`, no docker, no pip in this environment. CI is
the only way to compile. [.github/workflows/build.yml](.github/workflows/build.yml) just
calls ZMK's reusable `build-user-config.yml`, which reads
[build.yaml](build.yaml) as its job matrix.

```
push → Actions tab → newest run → download the "firmware" artifact
     → klor_left.uf2 / klor_right.uf2
     → double-tap reset on each half, drag the .uf2 onto the USB drive
```

`origin` is `git@github.com:c4ssini-1/zmk-config-klor.git` (SSH); `upstream` still points at
GEIGEIGEIST over HTTPS for pulling. Downloading run **logs** needs a token with
`Actions: read` — anonymous access returns *"Must have admin rights"* even though the repo
is public.

To wipe stored Bluetooth pairings, uncomment the `settings_reset` entry at the bottom of
[build.yaml](build.yaml), flash that image to **both** halves, then flash normal firmware back.

Since edits cannot be compiled, validate keymap changes mechanically instead — parse the
file and assert, rather than eyeballing the columns:

- every layer has exactly **44 bindings** and **2 `sensor-bindings`** entries, left
  encoder first;
- the number of `*_layer` nodes matches the number of `#define`s, and their order matches;
- every layer is still reachable by walking the `&mo` chain from BASE;
- no keycode is accidentally duplicated or dropped by a rearrangement.

Strip `/* ... */` comments before parsing — the ASCII diagrams are full of characters that
otherwise look like tokens. Keycode and behavior names can be checked against
`app/include/dt-bindings/zmk/*.h` and `app/dts/behaviors/*.dtsi` in a ZMK checkout at the
pinned revision.

**A green build is not proof the change took effect.** Kconfig silently ignores an option
whose dependencies are absent — no warning, no error. A build once shipped with neither the
LED driver nor the custom module in it because `LED_STRIP` had quietly vanished. After any
Kconfig change, read the **`<shield> - nice_nano_zmk Kconfig file`** step in the run log and
confirm the symbols are actually set, and grep the **`West Build`** step for your `.c`
filename to confirm it reached the compiler.

## File precedence — the thing to get right first

Two `klor.keymap` and two `klor.conf` files exist. ZMK's user-config build resolves shield
files from `config/` **before** the shield directory, so:

| Live (edit these) | Shadowed (inert, upstream leftovers) |
| :--- | :--- |
| [config/klor.keymap](config/klor.keymap) | `boards/shields/klor/klor.keymap` |
| [config/klor.conf](config/klor.conf) | `boards/shields/klor/klor.conf` |

`config/klor.conf` applies to **both** halves: ZMK matches the conf filename against the
shield *directory* name (`klor`), not the per-half shield names.

**For anything that must differ per half, use `boards/shields/klor/klor_{left,right}.conf`.**
Unlike the shield's `klor.conf`, these are *not* shadowed — the build log shows each half
merging its own. `klor_left.conf` carries the custom status screen, which would break the
right half if it were shared: `ZMK_DISPLAY_STATUS_SCREEN_CUSTOM` makes the firmware call
`zmk_display_status_screen()`, and the only implementation is gated on
`ZMK_SPLIT_ROLE_CENTRAL`, so the peripheral would fail to link. `klor_right.conf` is empty.

Also inert: `klor_status_screen.c`, `battery_status.c`, `output_status.c`,
`profile_status.c`, and `icons/`. They are guarded by `CONFIG_CUSTOM_WIDGET_*` symbols that
are defined nowhere, and there is no `CMakeLists.txt` in the shield to compile them. They
also still `#include <logging/log.h>` (pre-Zephyr-3.1 path). **ZMK's built-in status screen
is what actually runs.** Do not assume these files affect the display; reviving them means
adding a CMakeLists.txt, the Kconfig symbols, and fixing the include paths.

## Shield structure

`boards/shields/klor/` — a shield vendored into this repo so it can be edited without
forking ZMK.

The repo is a **Zephyr module**: [zephyr/module.yml](zephyr/module.yml) sets
`board_root: .` (so ZMK finds `boards/shields/klor`) plus `cmake: .` and `kconfig: Kconfig`
(so it can contribute C code). The shield used to live at `config/boards/`, which still
works but emits a CMake deprecation warning — the layout now matches ZMK's
`unified-zmk-config-template`. Do not move it back.

The devicetree include chain is:

```
klor_left.overlay ─┐
klor_right.overlay ┴→ klor.dtsi → klor_common.dtsi
```

- `klor_common.dtsi` — shared nodes: kscan (col2row, `wakeup-source`), both `alps,ec11`
  encoders declared `disabled`, the SSD1306 OLED on `pro_micro_i2c`, and the `chosen` block.
- `klor.dtsi` — the `default_transform` (12 cols × 4 rows) and row GPIOs. The two encoder
  push switches are `RC(3,5)` and `RC(3,6)`, sitting mid-row-3 between the halves — that is
  why polydactyl is 44 positions, not 42, and why the BOM lists 44 diodes.
- `klor_{left,right}.overlay` — per-half column GPIOs (mirrored order) and each half enables
  its own encoder. The right overlay adds `col-offset = <6>` so the right half's keys land in
  the back half of the transform.
- `boards/*.overlay` — per-controller RGB wiring (**SPI3** → WS2812 on P0.06, chain-length
  21). The filename **must** match the board target, hence
  `nice_nano_nrf52840_zmk.overlay`. Upstream used `spi1`, which never completes a transfer
  on this board — see "Known breakages" below. The `nrfmicro_*` overlays correctly keep
  `spi1` (different controller) and are unused by `build.yaml`.
- `Kconfig.defconfig` — sets `ZMK_SPLIT_ROLE_CENTRAL` for the left half only, plus LVGL
  tuning for a 1-bpp display.

## Keymap

[config/klor.keymap](config/klor.keymap) — **four** layers: `BASE` (QWERTY), `XTRA`
(calculator/symbols), `FN`, `SYS`.

**Layers chain, they do not combine.** `&mo XTRA` on BASE position 38 → `&mo FN` on XTRA
position 41 → `&mo SYS` on FN position 22. There is deliberately no `conditional_layers`
node: the old NAV+NUM tri-layer died with the NAV layer, because a conditional layer needs
two layers held at once and only one momentary layer key remains on BASE.

Two consequences that are easy to get wrong:

- **`&trans` on FN resolves to XTRA, not BASE.** FN is only ever entered through XTRA, so
  38 is still held and XTRA sits active underneath. FN's cleared right hand therefore
  exposes XTRA's numpad rather than doing nothing. Use `&none` for a genuinely dead key.
- **Layer indices are positional.** The `#define` values name the order of nodes inside
  `keymap`. Deleting or reordering a layer renumbers everything below it, so the define
  block and the node order must stay in sync — the layer count and define count should
  always match.

One combo, 50 ms: `<0 1>` sends Tab. Escape has its own key at position 10, and 22 is the
only Shift on the board.

**Super deliberately exists twice**, because the layer underneath decides what it can chord
with. Position 39 on BASE is `&spc LGUI SPACE` — tap Space, hold Super — which keeps the
alphas live, so `Super+T` works. Position 39 on XTRA is a plain `&kp LGUI`, reached by
holding 38 first, which puts the numpad underneath instead and serves `Super+1`. Neither
one alone covers both.

There *was* a `<38 39>` Super combo; it was removed. A combo needs both keys inside 50 ms
every time, which is poor for a modifier you hold through a chord, and it swallowed both
presses so XTRA never engaged.

`spc` is a separate hold-tap from `hm` rather than the built-in `&mt`. `&mt` defaults to
`hold-preferred`, which resolves to the hold the moment another key is pressed inside the
tapping term — on the space bar that turns "the quick" into `Super+q`. `tap-preferred` plus
`require-prior-idle-ms` is what makes a hold-tap survive on the most-pressed key.

Shifted glyphs are never bound as mod-morphs — ZMK sends HID usage codes and the host
applies shift, so `&kp BSLH` already yields `|` under shift. Only bind an explicit shifted
keycode when the base key is absent from the layer.

Two hold-taps are defined: `spc` (in use on BASE 39) and `hm`, a home-row-mod
variant that remains deliberately unused.

The file opens with a **position map** comment numbering all 44 bindings, and each layer
carries a hand-maintained ASCII diagram. Keep both in sync when bindings change.

## The OLED keymap display

[src/klor_status_screen.c](src/klor_status_screen.c) replaces ZMK's built-in status screen
on the **left half only** with a miniature map of the active layer. There is no room for
both on 128×64 at 1 bpp, and the peripheral cannot show a layer at all — ZMK encodes that
itself: `ZMK_WIDGET_LAYER_STATUS depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL`.

Only **this half's** keys are drawn — grid columns 0–5. The full 44-key grid fitted, but it
read as a dense block and half of it was under the other hand. Pressed keys are shown
**inverted**: a white square with the glyph repainted in black.

Implementation notes worth keeping:

- **It is a canvas, not labels.** A label has no per-character styling, so it cannot invert
  one glyph. Drawing into an `LV_COLOR_FORMAT_I1` canvas makes every pixel ours, and any
  number of keys can be lit at once. The font is still `lv_font_unscii_8`; monospace is what
  makes the grid line up.
- **Two I1 buffer traps.** An indexed draw buffer keeps its palette in the *first* bytes of
  the buffer with pixels after it, but `lv_canvas_set_buffer` sizes the buffer as
  `stride * h` only — so the array must be `palette + pixels` (8 + 1024 here) or LVGL writes
  past the end. And the palette must be set explicitly with `lv_canvas_set_palette`;
  blending picks an index by luminance threshold, so white lands on 1 and black on 0.
- **Geometry is budgeted on advances, not glyphs.** `unscii_8`'s `line_height` is 9, not 8,
  and LVGL advances by `letter_space` after *every* character, not just between them. Both
  have already caused visible bugs (bottom rows pushed off the panel; a scrollbar from
  overflow). A cell is 11 × 15, so the block is 121 × 60 inside 128 × 64.
- **The held-key set is accumulated in the state fetch, not the draw callback.**
  `ZMK_DISPLAY_WIDGET_LISTENER` keeps one state snapshot and submits a `k_work`; a second
  event arriving before that work runs finds the submit already pending and only the newer
  snapshot survives. A state of "which key just changed" would therefore drop events and
  leave highlights stuck on. Carrying the whole held set as a bitmask makes a coalesced
  update still correct.
- **The panel runs inverted, and the canvas must absorb it.** `klor_common.dtsi` declares
  the SSD1306 with `inversion-on`, so a pixel LVGL considers white lands on the glass dark.
  `klor_status_screen.c` therefore draws through `ON_GLASS_WHITE`/`ON_GLASS_BLACK`, which
  wrap the *opposite* LVGL colour. Do not "fix" this by dropping `inversion-on` — that node
  is shared with the right half, whose built-in screen is already correct against it. And
  `ZMK_DISPLAY_INVERT` is not the lever: it only passes a dark-background flag to
  `lv_theme_mono_init`, which styles widgets, and a canvas never consults the theme.
- **The screen must not render on the system work queue.** ZMK defaults to
  `ZMK_DISPLAY_WORK_QUEUE_SYSTEM`, which runs UI updates on the same queue that processes
  key events and sends HID reports. Harmless when the screen only redrew on a layer change;
  **not** harmless once it redraws on every press and release, because a full canvas repaint
  plus a full-panel I2C flush (1 KB at 400 kHz) then sits in front of the next keystroke.
  This shipped once and cost roughly three keys in ten plus a stuck Shift from a delayed
  release — a keyboard fault with no trace in the build log. `klor_left.conf` sets
  `ZMK_DISPLAY_WORK_QUEUE_DEDICATED` so a slow redraw can only lag the display.
- **Never repaint when the picture would not change.** Both widget callbacks compare against
  the current state and return early. The central sees the peripheral's key events too, and
  those positions are in columns 6–11 which are never drawn, so they are filtered out at the
  event rather than allowed to trigger an identical repaint.
- **`LV_USE_CANVAS` is `select`ed, not relied on.** lvgl only defaults it on when
  `LV_CONF_MINIMAL` is off. `LV_USE_IMAGE` is selected alongside it because lvgl merely
  `imply`s that from canvas, and an `imply` can be overridden to `n` — which fails to link
  rather than warn.
- **The glyphs are generated, not hand-written.** `src/keymap_glyphs.h` comes from
  [scripts/gen_keymap_glyphs.py](scripts/gen_keymap_glyphs.py), which parses the keymap and
  the matrix transform. **Re-run it after any keymap change** or the display will
  confidently show the wrong keys. It resolves `&trans` down the layer chain, so what is
  drawn is what the key actually does. It also emits `keymap_pos_row[]` / `keymap_pos_col[]`,
  the inverse lookup the highlight needs to turn a key position from a
  `zmk_position_state_changed` event into a grid cell — from the same transform, so the
  picture and the highlight cannot disagree about where a key lives.
- **Switching to a custom screen silently drops font and theme config.** ZMK sets
  `LV_FONT_MONTSERRAT_*`, the default font and `LV_USE_THEME_MONO` inside
  `if ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN`. Choosing the custom screen loses all of it, so
  `klor_left.conf` asks for `LV_FONT_UNSCII_8`, `LV_FONT_DEFAULT_UNSCII_8` and
  `LV_USE_THEME_MONO` explicitly. The same applies to `LV_Z_MEM_POOL_SIZE`, which ZMK only
  defaults to 4096 for the built-in screen — it is set to 8192 here because every
  `lv_draw_*` call mallocs a task plus a copy of its descriptor and they all queue until
  `lv_canvas_finish_layer` dispatches, so peak use scales with how many keys are held. The
  1 KB canvas buffer is a static array and does *not* come from this pool.

## Per-key reactive underglow

[src/klor_rgb_reactive.c](src/klor_rgb_reactive.c) — white underglow where the LED under a
pressed key lifts from 10% to 60% and drops back on release. Mainline ZMK cannot do this,
so the module **owns the `led_strip` device outright**.

Consequences, all deliberate:

- **`CONFIG_ZMK_RGB_UNDERGLOW=n` is mandatory.** Two writers on one strip would fight.
- **`CONFIG_LED_STRIP=y` must be set explicitly.** `ZMK_RGB_UNDERGLOW` was the only thing
  doing `select LED_STRIP`; turning it off silently takes the whole strip subsystem with it,
  including `WS2812_STRIP_SPI`, whose Kconfig is sourced inside `if LED_STRIP`.
- **The `&rgb_ug` keys on SYS are inert.** They still *build* — the behavior node is
  declared unconditionally in ZMK's `behaviors.dtsi` and only the driver is gated — but
  nothing implements effects, hue or saturation any more. Add them to the module rather than
  re-enabling ZMK's underglow.

**Each half is self-contained; nothing crosses the split link.**
`zmk_position_state_changed` is raised by `physical_layouts.c` off the *local* matrix scan
on both boards, tagged `ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL`. Filtering on that source
means each half reacts only to its own keys. The central also sees peripheral keys, arriving
with a different source, and must ignore them — otherwise a right-hand press lights an
unrelated left-hand LED.

**`CMakeLists.txt` must use `target_sources_ifdef(... app ...)`, not `zephyr_library()`.**
ZMK declares its headers with `target_include_directories(app PRIVATE include)`, so
`app/include` — holding `zmk/event_manager.h` and `zmk/events/*` — is invisible to a
standalone library. Compiling into `app` inherits those paths, which is what the established
community modules do.

### The LED map

Measured on hardware, not read off the diagram. Both halves chain **identically in mirrored
coordinates**, every switch maps to exactly one LED, and the **encoder pushes (28, 29) have
no LED**.

```
LED   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20
left 39  38  37  27  15   4   3  14  26  36  25  13   2   1  12  24  23  11   0  10  22
right 40 41  42  30  16   5   6  17  31  43  32  18   7   8  19  33  34  20   9  21  35
```

The chain is a boustrophedon: inner thumbs, up the innermost column, down the next, with the
outer thumb spliced in at index 9, working outward to the pinky.

If the map ever needs re-deriving, the method that worked was a throwaway harness walking one
lit LED along the strip while USB serial logged the index, correlated against `/dev/input`
keypresses. Keys that cannot emit a keycode — dead ones, and layer keys like `&mo` — leave
holes that must be filled by elimination and cross-checked against the other half.

## Pinning and upstream drift

[config/west.yml](config/west.yml) pins ZMK to a specific SHA (`6e2ef41…`, 2026-08-11).
Upstream tracked `main` unpinned, which is exactly how this config rotted. Bump the SHA
deliberately, never casually, and re-verify via CI.

Known breakages already fixed (documented in [readme.md](readme.md) and the comments in each
file) — do not regress them:

- board target `nice_nano_v2` → `nice_nano//zmk` (Zephyr hardware-model-v2)
- `ZMK_SPLIT_BLE_ROLE_CENTRAL` → `ZMK_SPLIT_ROLE_CENTRAL` — **silent failure**: the old name
  still "builds", it just yields two peripherals and a dead keyboard
- layer `label` → `display-name`
- devicetree node `label` properties removed from the bindings
- `CONFIG_WS2812_STRIP` **deleted, not renamed** — Zephyr 4.1 split it into
  `WS2812_STRIP_SPI` / `_I2S` / `_GPIO`. Assigning the old name is a *hard error* that
  aborts the build. Nothing replaces it: `WS2812_STRIP_SPI` is `default y` once a
  `worldsemi,ws2812-spi` node exists in the devicetree.
- `&spi1` → `&spi3` for the LED strip — **silent failure**: builds fine, then every
  transfer dies with `spi_nrfx_spim: Timeout waiting for transfer complete` and
  `Failed to update the RGB strip (-116)`. On the nRF52840, SPIM0/1/2 share a hardware
  instance with TWIM/TWIS/SPIS while SPIM3 is standalone; all 27 nice!nano shields in the
  ZMK tree use `spi3`.
- `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=y` **kills both OLEDs** on this board — see the long
  comment in [config/klor.conf](config/klor.conf). They share the external power rail with
  the LED strip, and the idle handler cuts that rail without ever restoring it.

ZMK still honours `chosen zmk,matrix_transform`, so a `zmk,physical-layout` node is only
required if ZMK Studio support is wanted.

## Hardware capability limits under ZMK

The KLOR PCB's capabilities are a superset of what ZMK delivers. Do not add config for these:

- **Speaker/buzzer and haptic (DRV2605): no ZMK driver.** QMK only.
- **Per-key RGB needs custom code, and this repo has it.** The 21 SK6812 minis per half
  are one addressable strip, but they sit one-per-key, so per-key lighting is physically
  possible. *Mainline ZMK cannot do it* — four whole-strip effects, no per-LED API, and the
  underglow subsystem subscribes only to `zmk_activity_state_changed`. See "Per-key reactive
  underglow" below for the module that does.
- **The right OLED cannot show the active layer.** It is a BLE peripheral and receives no
  layer state without custom firmware work.
- **Both encoders do work.** Upstream's "the secondary encoder doesn't work" note is stale —
  ZMK forwards peripheral sensor events.

## Conventions

- Comments in this fork explain *why* a value is set, especially where a wrong value fails
  silently. Match that density when editing config files.
- Markdown headings in [readme.md](readme.md) are ALL CAPS.
- Commit messages are imperative subject lines with a body explaining what broke and why.

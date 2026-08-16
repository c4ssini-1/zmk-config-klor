# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A **ZMK user config** — not a firmware source tree. It contains a keymap, a Kconfig
fragment, and a vendored copy of the `klor` shield definition. ZMK itself is fetched at
build time by `west` per [config/west.yml](config/west.yml); nothing in this repo compiles
on its own.

Forked from `GEIGEIGEIST/zmk-config-klor` and repaired for current ZMK. Targets a
**polydactyl KLOR (44 positions), BLE, two nice!nano v2 controllers, OLEDs on both halves**.
Hardware design files live in the separate `GEIGEIGEIST/KLOR` repo.

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

Note the repo currently has only an `upstream` remote pointing at GEIGEIGEIST; a personal
`origin` is needed for CI to run on pushes.

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

## File precedence — the thing to get right first

Two `klor.keymap` and two `klor.conf` files exist. ZMK's user-config build resolves shield
files from `config/` **before** the shield directory, so:

| Live (edit these) | Shadowed (inert, upstream leftovers) |
| :--- | :--- |
| [config/klor.keymap](config/klor.keymap) | `config/boards/shields/klor/klor.keymap` |
| [config/klor.conf](config/klor.conf) | `config/boards/shields/klor/klor.conf` |

`config/klor.conf` applies to **both** halves: ZMK matches the conf filename against the
shield *directory* name (`klor`), not the per-half shield names. `klor_left.conf` and
`klor_right.conf` exist but are empty.

Also inert: `klor_status_screen.c`, `battery_status.c`, `output_status.c`,
`profile_status.c`, and `icons/`. They are guarded by `CONFIG_CUSTOM_WIDGET_*` symbols that
are defined nowhere, and there is no `CMakeLists.txt` in the shield to compile them. They
also still `#include <logging/log.h>` (pre-Zephyr-3.1 path). **ZMK's built-in status screen
is what actually runs.** Do not assume these files affect the display; reviving them means
adding a CMakeLists.txt, the Kconfig symbols, and fixing the include paths.

## Shield structure

`config/boards/shields/klor/` — a shield vendored into the user config so it can be edited
without forking ZMK. The devicetree include chain is:

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
- `boards/*.overlay` — per-controller RGB wiring (SPI1 → WS2812, chain-length 21). The
  filename **must** match the board target, hence `nice_nano_nrf52840_zmk.overlay`. The
  `nrfmicro_*` overlays are carried from upstream and unused by `build.yaml`.
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

Two combos, both 50 ms: `<0 1>` sends Tab, and `<38 39>` sends `&kp LGUI`, scoped with
`layers = <BASE>` so it cannot fire out of the other layers. A ZMK combo holds its binding
while the trigger keys stay held, so the Super combo works as a real modifier, not a tap.
Escape has its own key at position 10, and 22 is the only Shift on the board.

Shifted glyphs are never bound as mod-morphs — ZMK sends HID usage codes and the host
applies shift, so `&kp BSLH` already yields `|` under shift. Only bind an explicit shifted
keycode when the base key is absent from the layer.

A `hm` home-row-mod hold-tap is defined but deliberately unused.

The file opens with a **position map** comment numbering all 44 bindings, and each layer
carries a hand-maintained ASCII diagram. Keep both in sync when bindings change.

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

ZMK still honours `chosen zmk,matrix_transform`, so a `zmk,physical-layout` node is only
required if ZMK Studio support is wanted.

## Hardware capability limits under ZMK

The KLOR PCB's capabilities are a superset of what ZMK delivers. Do not add config for these:

- **Speaker/buzzer and haptic (DRV2605): no ZMK driver.** QMK only.
- **RGB is whole-strip underglow only.** The 21 SK6812 minis per half are one addressable
  strip; mainline ZMK has no per-key or per-layer control despite "RGB matrix" naming.
- **The right OLED cannot show the active layer.** It is a BLE peripheral and receives no
  layer state without custom firmware work.
- **Both encoders do work.** Upstream's "the secondary encoder doesn't work" note is stale —
  ZMK forwards peripheral sensor events.

## Conventions

- Comments in this fork explain *why* a value is set, especially where a wrong value fails
  silently. Match that density when editing config files.
- Markdown headings in [readme.md](readme.md) are ALL CAPS.
- Commit messages are imperative subject lines with a body explaining what broke and why.

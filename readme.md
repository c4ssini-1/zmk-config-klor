<picture>
  <source media="(prefers-color-scheme: dark)" srcset="/docs/images/klor-font-logo-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="/docs/images/klor-font-logo-bright.svg">
  <img alt="KLOR logo font" src="/docs/images/klor-font-logo-bright.svg">
</picture>

# ZMK CONFIG FOR THE KLOR SPLIT KEYBOARD

Forked from [GEIGEIGEIST/zmk-config-klor](https://github.com/GEIGEIGEIST/zmk-config-klor).
Hardware files and build guides: [GEIGEIGEIST/KLOR](https://github.com/GEIGEIGEIST/KLOR).

This fork is set up for a **polydactyl layout, BLE build, two nice!nano v2 controllers,
OLEDs on both halves**, with a QWERTY keymap.

## WHAT CHANGED FROM UPSTREAM

Upstream was last updated in June 2024 while tracking ZMK `main` unpinned, so it no
longer built. Fixed here:

| Change | Why |
| :--- | :--- |
| `build.yaml` board is now `nice_nano//zmk` | ZMK moved to Zephyr hardware-model-v2 board targets; `nice_nano_v2` no longer exists and is not aliased |
| `ZMK_SPLIT_BLE_ROLE_CENTRAL` → `ZMK_SPLIT_ROLE_CENTRAL` | Renamed when wired split landed. Under the old name the left half silently built as a peripheral, so the keyboard would not work |
| Board overlay renamed to `nice_nano_nrf52840_zmk.overlay` | Must match the new board target name. v1 vs v2 is now a board revision, so the separate v1 overlay was removed |
| Layer `label` → `display-name` | ZMK renamed the property; this is what the OLED layer widget reads |
| `label` properties dropped from devicetree nodes | Removed from the Zephyr bindings |
| `wakeup-source` added to the kscan node | Needed to wake from deep sleep on a keypress |
| `west.yml` pinned to a ZMK commit | Upstream tracked `main` unpinned, which is how it rotted. Bump the SHA deliberately when you want newer ZMK |
| Both encoders bound | ZMK forwards sensor events from the peripheral half now, so the right encoder works — upstream's "known issue" is out of date |
| `CONFIG_WS2812_STRIP` removed | Zephyr 4.1 split it into per-transport symbols. The old name is undefined, and assigning an undefined symbol aborts the build. `WS2812_STRIP_SPI` turns itself on from the devicetree |
| EC11 `resolution` → `steps` | `resolution` is deprecated. Different units — pulses per detent vs per rotation — so `<4>` becomes `<80>`. Behaviour is unchanged |
| Shield moved to `boards/` + `zephyr/module.yml` | `config/boards` is deprecated; the repo is now a Zephyr module, matching ZMK's `unified-zmk-config-template` |
| LED strip moved from `&spi1` to `&spi3` | **Silent failure.** Builds fine, then every transfer times out with `-ETIMEDOUT`. On the nRF52840, SPIM0/1/2 share a hardware instance with TWIM/TWIS/SPIS; SPIM3 is standalone. All 27 nice!nano shields in the ZMK tree use `spi3` |
| `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER` forced to `n` | It gates the external power rail that the **OLEDs** share with the LED strip. ZMK cuts that rail when underglow goes idle and never restores it, so both screens die and stay dead |
| ZMK's underglow replaced with a custom module | Per-key reactive lighting, which mainline ZMK cannot do — see below |

## WHAT THE KLOR HARDWARE CAN AND CANNOT DO UNDER ZMK

- **Speaker / buzzer: not supported.** ZMK has no speaker driver. QMK only.
- **Haptic feedback (DRV2605): not supported.** ZMK has no haptic driver. QMK only.
- **Per-key RGB works here, via custom firmware.** The 21 SK6812 minis per half sit
  one per key, so per-key lighting is physically possible — but *mainline ZMK cannot
  do it*: four whole-strip effects, no per-LED API, and no key-event hook. This repo
  ships [`src/klor_rgb_reactive.c`](src/klor_rgb_reactive.c), which takes over the
  strip and does it properly. See RGB below.
- **The right OLED is limited.** It is a BLE peripheral and does not receive layer
  state, so it cannot show the active layer without custom firmware work.

## KEYMAP

Four layers, defined in [`config/klor.keymap`](config/klor.keymap). They **chain** rather
than combine — each one is reached from the one before it, adding a key:

| Layer | How to reach it | What's on it |
| :--- | :--- | :--- |
| `BASE` | default | QWERTY. Esc on the left outer column, Enter on the right, brackets filling the right hand's bottom row |
| `XTRA` | hold **38** (left inner thumb) | Calculator on the right hand — numpad plus `+ - * / =`. Symbols on the left in QWERTY number-row order |
| `FN` | from `XTRA`, add **41** | F1–F12. F2–F11 straight across the top row, F1 and F12 on the outer columns below |
| `SYS` | from `FN`, add **22** | Bluetooth profiles, USB/BLE output, bootloader, reset. *The RGB keys here are inert* — see LIGHTING |

```
BASE
  ┌────┬────┬────┬────┬────┐              ┌────┬────┬────┬────┬────┐
  │ Q  │ W  │ E  │ R  │ T  │              │ Y  │ U  │ I  │ O  │ P  │
┌─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┐        ┌──┴─┬──┴─┬──┴─┬──┴─┬──┴─┬──┴─┐
│ESC │ A  │ S  │ D  │ F  │ G  │        │ H  │ J  │ K  │ L  │ ]  │BSPC│
├────┼────┼────┼────┼────┼────┤ ╭────╮╭────╮ ├────┼────┼────┼────┼────┼────┤
│SHFT│ Z  │ X  │ C  │ V  │ B  │ │MUTE││PLAY│ │ N  │ M  │ [  │ ;  │ \  │ENTR│
└────┴────┴────┼────┼────┼────┤ ╰────╯╰────╯ ├────┼────┼────┼────┴────┴────┘
               │CTRL│ALT │XTRA│ │SPACE│ │ ,  │ .  │ /  │ '  │
               └────┴────┴────┘ └─────┘ └────┴────┴────┴────┘

XTRA                                          FN
  `   ~   !   @   #      1   2   3   0   +     F2  F3  F4  F5  F6    F7  F8  F9 F10 F11
      $   %   ^   &   _      4   5   6   -  *  F1                                    F12
      (   )                  7   8   9   /  =  SYS
```

There is only **one Shift**, on position 22 — the right Shift was given up to make room for
Enter.

**Super has two routes**, and the difference matters for window-manager bindings:

| Gesture | Layer underneath | Good for |
| :--- | :--- | :--- |
| **hold 39** | BASE — the alphas | `Super+T`, `Super+Q`… |
| **hold 38, then 39** | XTRA — the numpad | `Super+1` … `Super+0` |

Position 39 is a hold-tap: **tap for Space, hold for Super**. It is not the built-in `&mt`,
whose default `hold-preferred` flavour would turn "the quick" into `Super+q`. The `spc`
behaviour uses `tap-preferred` plus `require-prior-idle-ms`, so a hold only registers when
you have not just been typing.

**Tab** is the `Q + W` combo (positions 0 + 1), 50 ms window — the only combo left.

Shifted glyphs need no special behavior: ZMK sends HID usage codes and the *host* applies
shift, so `&kp BSLH` already yields `|`, `&kp LBKT` yields `{`, and so on.

### Encoders

| Layer | Left (28) | Right (29) |
| :--- | :--- | :--- |
| `BASE` | Volume | Page up/down |
| `XTRA` | Screen brightness | Page up/down |
| `FN` | Volume | Track next/previous |
| `SYS` | Volume | Page up/down |

The push switches are ordinary keymap positions: 28 is Mute and 29 is Play/Pause on `BASE`.

### A note on fall-through

`FN` can only be reached *through* `XTRA`, so position 38 stays held and `XTRA` stays
active underneath. Every `&trans` on `FN` therefore resolves to `XTRA`, not to `BASE` — in
practice `FN`'s right hand gives you F-keys **and** a live numpad at the same time. Use
`&none` instead of `&trans` if you ever want a key on `FN` to genuinely do nothing.

The keymap file opens with a position map numbering all 44 positions (42 keys plus
the two encoder push switches). Use those numbers when adding combos.

## LIGHTING

Per-key reactive underglow, from [`src/klor_rgb_reactive.c`](src/klor_rgb_reactive.c):
**white at 10%, and the LED under a pressed key rises to 60% until you let go.**

Mainline ZMK cannot do this — it has four whole-strip effects, no per-LED API, and the
underglow subsystem never sees key events. So the module takes the strip over completely,
which has three consequences worth knowing:

- `CONFIG_ZMK_RGB_UNDERGLOW` is **off**. Two writers on one strip would fight.
- The `&rgb_ug` keys on `SYS` **do nothing**. Effects, hue and saturation are not
  implemented. Add them to the module rather than switching ZMK's underglow back on.
- The encoder pushes have no LED, so nothing lights when you click them.

**Each half is independent.** ZMK raises key events locally on both boards, so the right
half lights its own keys without anything crossing the Bluetooth link — no latency, and no
dependence on the halves being connected.

Brightness is deliberately capped. White drives all three dies in every LED, and this board
browns out above roughly 70% — green and blue starve before red, so white drifts pink. Only
a few LEDs are bright at once here, so the load stays well under that, but the ceiling is
real if you raise the numbers.

## DISPLAY

**Both** OLEDs show a miniature map of the keymap instead of ZMK's battery and profile
widgets — there is no room for both on 128×64 at 1 bpp. Each half draws only its own keys,
spread across the whole panel with a dot lattice between them:

```
 . Q . W . E . R . T
 e . A . S . D . F . G
 s . Z . X . C . V . B
 . c . a . x .   . m
```

**The key you press is shown inverted** — a white square with the glyph in black — and any
number of keys can be highlighted at once. That needs per-pixel control, so the screen is
drawn into a 1-bpp LVGL canvas rather than built from labels.

The glyphs are generated from the keymap by
[`scripts/gen_keymap_glyphs.py`](scripts/gen_keymap_glyphs.py). **Re-run it after any keymap
change** or the display will confidently show the wrong keys.

The **right** OLED also shows the state of the split link in the last cell of its bottom
row, just after the apostrophe: a **tick** when it has found the left half, a **cross** when
it has not. Handy for telling "the halves have not paired" apart from "a key is dead".

**The right OLED does not follow the layer.** It is a BLE peripheral and never runs the
keymap — ZMK does not even compile the keymap or the layer event into a peripheral build, so
this is a link-time limit rather than something that can be switched on. The right half
highlights its own keys correctly but always draws the BASE glyphs. Carrying the layer
across the split link needs a custom channel and is not done yet.

## FLASHING

CI is the only way to build this — there is no local toolchain. `origin` is this fork;
`upstream` still points at GEIGEIGEIST for pulling changes down.

- push to this repo, then open the **Actions** tab on GitHub
- open the newest run and download the `firmware` artifact
- unzip it: you get `klor_left.uf2` and `klor_right.uf2`
- connect the left half by USB, press reset twice — it mounts as a USB drive
- drag `klor_left.uf2` onto the drive; it reboots on its own
- repeat with the right half and `klor_right.uf2`

If the halves refuse to talk to each other after a big firmware change, uncomment the
`settings_reset` entry in `build.yaml`, flash that image to **both** halves, then flash
the normal firmware back.

## NOT DONE YET

- **Layer sync to the right OLED.** The right half highlights its own keys but always draws
  BASE, so it shows QWERTY while you are on `XTRA` — where the right hand is actually a
  numpad. The split link has no layer channel, so this needs a custom one: a behaviour
  fired from a layer-change listener on the central, carried by
  `zmk_split_central_invoke_behavior()` and received by name on the peripheral.
- **Repair the left controller.** Its **P0.09** pin is broken — that is matrix column 5,
  so `T`, `G`, `B` and the left encoder push are dead, along with the four LEDs under
  them. While the iron is out, check **VCC (pin 21)** and the **grounds (pins 3, 4, 23)**:
  the left half browns out on white where the right half does not, which points at a
  high-resistance power joint.
- **ZMK Studio** (live keymap editing over USB, no reflash) needs a `zmk,physical-layout`
  node describing the polydactyl key positions. The shield still uses the older
  `zmk,matrix_transform`, which ZMK continues to honour.

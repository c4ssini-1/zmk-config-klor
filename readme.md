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

## WHAT THE KLOR HARDWARE CAN AND CANNOT DO UNDER ZMK

- **Speaker / buzzer: not supported.** ZMK has no speaker driver. QMK only.
- **Haptic feedback (DRV2605): not supported.** ZMK has no haptic driver. QMK only.
- **RGB is whole-strip only.** The KLOR wires its SK6812 minis as an addressable
  strip, but mainline ZMK drives it as underglow effects across the whole strip.
  There is no per-key or per-layer control despite the "RGB matrix" naming.
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
| `SYS` | from `FN`, add **22** | Bluetooth profiles, USB/BLE output, RGB, bootloader, reset |

```
BASE
  ┌────┬────┬────┬────┬────┐              ┌────┬────┬────┬────┬────┐
  │ Q  │ W  │ E  │ R  │ T  │              │ Y  │ U  │ I  │ O  │ P  │
┌─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┐        ┌──┴─┬──┴─┬──┴─┬──┴─┬──┴─┬──┴─┐
│ESC │ A  │ S  │ D  │ F  │ G  │        │ H  │ J  │ K  │ L  │ ]  │BSPC│
├────┼────┼────┼────┼────┼────┤ ╭────╮╭────╮ ├────┼────┼────┼────┼────┤
│SHFT│ Z  │ X  │ C  │ V  │ B  │ │MUTE││PLAY│ │ N  │ M  │ [  │ ;  │ \  │ENTR│
└────┴────┴────┼────┼────┼────┤ ╰────╯╰────╯ ├────┼────┼────┼────┴────┘
               │CTRL│ALT │XTRA│ │SPACE│ │ ,  │ .  │ /  │ '  │
               └────┴────┴────┘ └─────┘ └────┴────┴────┴────┘

XTRA                                          FN
  `   ~   !   @   #      1   2   3   0   +     F2  F3  F4  F5  F6    F7  F8  F9 F10 F11
      $   %   ^   &   _      4   5   6   -  *  F1                                    F12
      (   )                  7   8   9   /  =  SYS
```

There is only **one Shift**, on position 22 — the right Shift was given up to make room for
Enter. **Super** is not a key either: it is the `XTRA + Space` combo on positions 38 + 39.
A ZMK combo holds its binding for as long as the trigger keys are held, so that behaves as
a real modifier — hold both thumb keys and press L for Super+L.

**Tab** is the `Q + W` combo (positions 0 + 1). Both combos use a 50 ms window.

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

## FLASHING

CI is the only way to build this — there is no local toolchain. Note the repo currently has
only an `upstream` remote pointing at GEIGEIGEIST, so a personal `origin` has to be added
before a push will build anything.

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

- **None of this has been built or flashed yet.** The keymap is internally consistent —
  44 bindings and 2 sensor-bindings per layer, every layer reachable — but it has never
  been through CI or onto hardware.
- **ZMK Studio** (live keymap editing over USB, no reflash) needs a `zmk,physical-layout`
  node describing the polydactyl key positions. The shield still uses the older
  `zmk,matrix_transform`, which ZMK continues to honour.
- Per-layer keymap display on the OLEDs.
- Pixart Paw3204 trackball support.

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

Five layers, defined in [`config/klor.keymap`](config/klor.keymap):

| Layer | How to reach it | What's on it |
| :--- | :--- | :--- |
| `BASE` | default | QWERTY, both shifts on the outer columns |
| `NAV` | hold left thumb layer key | Arrows on H J K L, Home/End/PgUp/PgDn, undo/cut/copy/paste, mirrored mods on the left hand |
| `NUM` | hold right thumb layer key | Digits 1-0 across the home row, shifted symbols above, brackets and operators below |
| `FN` | hold **both** layer keys | F1-F12, media and brightness |
| `SYS` | from `FN`, hold the outer-left bottom key | Bluetooth profiles, USB/BLE output, RGB, bootloader, reset |

Escape is the **Q + W combo** — there is no room for a dedicated key.

The keymap file opens with a position map numbering all 44 positions (42 keys plus
the two encoder push switches). Use those numbers when adding combos.

## FLASHING

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

- **ZMK Studio** (live keymap editing over USB, no reflash) needs a `zmk,physical-layout`
  node describing the polydactyl key positions. The shield still uses the older
  `zmk,matrix_transform`, which ZMK continues to honour.
- Per-layer keymap display on the OLEDs.
- Pixart Paw3204 trackball support.

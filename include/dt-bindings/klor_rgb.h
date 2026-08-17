/*
 * Parameters for &rgb, the KLOR reactive underglow control behavior.
 *
 * Reachable from the keymap because zephyr/module.yml sets dts_root, which puts
 * this directory on the devicetree preprocessor's include path.
 */

#pragma once

#define KLOR_RGB_OFF 0
#define KLOR_RGB_ON  1

/*
 * Resolved to OFF or ON by the central before the command reaches the
 * peripheral, so the two halves cannot end up disagreeing about the state.
 */
#define KLOR_RGB_TOG 2

/*
 * &rgb -- the on/off switch for the reactive underglow.
 *
 * ZMK's &rgb_ug is not usable here. This config sets ZMK_RGB_UNDERGLOW=n so
 * src/klor_rgb_reactive.c can own the led_strip device outright, which leaves
 * every &rgb_ug binding inert: the behavior node is declared unconditionally in
 * ZMK's behaviors.dtsi and only the driver is gated, so those keys still build
 * and simply do nothing. This replaces them.
 *
 * It offers on, off and toggle and nothing else, because the module's scheme --
 * white, 10% idle, 60% under a pressed key -- has no effects, hue or saturation
 * to cycle through.
 *
 * BEHAVIOR_LOCALITY_GLOBAL is what makes one keypress reach both halves. ZMK
 * runs a global behavior locally and also ships it over the split link's
 * RUN_BEHAVIOR channel, where the peripheral looks the behavior up by name --
 * which is why this file must compile on both halves, not just the central.
 *
 * TOGGLE IS RESOLVED BEFORE IT CROSSES THE LINK. Each half keeps its own
 * enabled flag, so if both were simply told to "flip", any command one half
 * missed would leave the two permanently disagreeing. Instead the central turns
 * TOGGLE into an explicit ON or OFF in
 * binding_convert_central_state_dependent_params -- a hook ZMK provides for
 * exactly this -- so both sides are told the same absolute state. This is how
 * ZMK's own &ext_power handles the identical problem.
 */

#define DT_DRV_COMPAT klor_behavior_rgb

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/klor_rgb.h>

#include "klor_rgb.h"

/* Shares the reactive module's log module -- the two files are one feature. */
LOG_MODULE_DECLARE(klor_rgb_reactive, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_convert_central_state(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    if (binding->param1 == KLOR_RGB_TOG) {
        binding->param1 = klor_rgb_is_enabled() ? KLOR_RGB_OFF : KLOR_RGB_ON;
    }
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case KLOR_RGB_OFF:
        klor_rgb_set_enabled(false);
        return ZMK_BEHAVIOR_OPAQUE;
    case KLOR_RGB_ON:
        klor_rgb_set_enabled(true);
        return ZMK_BEHAVIOR_OPAQUE;
    case KLOR_RGB_TOG:
        /* Only reached if the convert hook did not run -- it is central-only,
         * so a peripheral-side invocation could still see TOGGLE. Flipping the
         * local flag is the best available answer there. */
        klor_rgb_set_enabled(!klor_rgb_is_enabled());
        return ZMK_BEHAVIOR_OPAQUE;
    default:
        LOG_ERR("unknown underglow command: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api klor_rgb_driver_api = {
    .binding_convert_central_state_dependent_params = on_convert_central_state,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &klor_rgb_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

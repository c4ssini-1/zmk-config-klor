/*
 * Tells the right half which layer is active.
 *
 * THE PROBLEM. The peripheral never runs the keymap -- ZMK does not even
 * compile src/keymap.c or src/events/layer_state_changed.c into a non-central
 * build -- so it cannot know a layer is held. Its OLED therefore drew BASE
 * forever, which is worst exactly where it matters: XTRA turns the right hand
 * into a numpad, so the one moment you most want the picture is the moment it
 * was wrong.
 *
 * THE CHANNEL. ZMK's split link carries positions, sensor events and input
 * events upward, and behaviors, HID indicators and physical-layout selection
 * downward. There is no layer channel and no way to add a characteristic from
 * a user config. Of the three downward hooks, only RUN_BEHAVIOR carries an
 * arbitrary value, so the layer index rides in a behavior's param1.
 *
 * Nothing binds this behavior to a key. The central invokes it directly from a
 * zmk_layer_state_changed listener via zmk_split_central_invoke_behavior(),
 * which queues a GATT write on the transport's own thread -- so calling it from
 * an event listener does not block the event.
 *
 * WHAT THIS DOES NOT DO. The write is unacknowledged
 * (bt_gatt_write_without_response), and there is no central-side event for "a
 * peripheral connected" to resync against. So a dropped packet, or a peripheral
 * that reboots while a layer is held, leaves the right screen stale. It is
 * self-correcting rather than reliable: zmk_layer_state_changed fires on both
 * activation and deactivation, so the next layer key press or release puts it
 * right, and the peripheral starts at BASE, which is what the central is
 * showing almost all of the time anyway.
 *
 * The node name is 7 characters. That is not cosmetic -- see klor_common.dtsi:
 * the name is what crosses the link and the payload field truncates to 8.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(klor_layer_sync, LOG_LEVEL_INF);

#define LAYER_SYNC_NODE DT_NODELABEL(layer_sync)

/* ------------------------------------------------------------------------
 * Peripheral side: receive the layer and hand it to the screen.
 * ------------------------------------------------------------------------ */

#define DT_DRV_COMPAT klor_behavior_layer_sync

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "klor_status.h"
#endif

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    klor_status_set_layer((uint8_t)binding->param1);
#else
    /* The central never invokes this on itself -- it reads the keymap. Reaching
     * here would mean someone bound it to a key by mistake. */
    LOG_WRN("layer sync invoked on the central; it is a peripheral-only sink");
#endif
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api klor_layer_sync_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    /* CENTRAL, not GLOBAL: this is never invoked through the keymap, so there is
     * no locality decision for ZMK to make. The central calls the split
     * function itself. */
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &klor_layer_sync_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */

/* ------------------------------------------------------------------------
 * Central side: watch the keymap and push every change outward.
 * ------------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

static uint8_t last_sent = 0xFF;

static void send_layer(uint8_t layer) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(LAYER_SYNC_NODE),
        .param1 = layer,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };

    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        int rc = zmk_split_central_invoke_behavior(i, &binding, event, true);
        /* -ENODEV simply means no peripheral is connected yet, which is normal
         * at boot and while the halves are apart. Not worth logging as an
         * error every time a layer changes. */
        if (rc && rc != -ENODEV) {
            LOG_WRN("layer sync to peripheral %d failed: %d", i, rc);
        }
    }
}

static int on_layer_state_changed(const zmk_event_t *eh) {
    uint8_t layer = (uint8_t)zmk_keymap_highest_layer_active();

    /* zmk_layer_state_changed fires for activation and deactivation both, and
     * the highest active layer is often unchanged afterwards. Skipping those
     * keeps a BLE write off the link for every no-op transition. */
    if (layer == last_sent) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    last_sent = layer;

    send_layer(layer);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(klor_layer_sync, on_layer_state_changed);
ZMK_SUBSCRIPTION(klor_layer_sync, zmk_layer_state_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

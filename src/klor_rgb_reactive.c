/*
 * Per-key reactive underglow for the KLOR.
 *
 * White everywhere at an idle level; the LED under a pressed key lifts to a
 * brighter level and drops back on release.
 *
 * Mainline ZMK cannot do this. Its underglow exposes no per-LED API -- the
 * pixel buffer is static inside rgb_underglow.c -- and it subscribes only to
 * zmk_activity_state_changed, never to key events. So this module owns the
 * led_strip device outright, which is why CONFIG_ZMK_RGB_UNDERGLOW must be n.
 *
 * SPLIT DESIGN: each half is entirely self-contained. zmk_position_state_changed
 * is raised locally by physical_layouts.c off the local matrix scan, on the
 * peripheral just as much as on the central, tagged
 * ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL. Filtering on that means each board
 * reacts only to its own keys and lights only its own strip. Nothing crosses
 * the split link, so there is no latency and no dependency on the connection
 * being up.
 *
 * The central also sees peripheral keys, arriving with a different source, and
 * those are deliberately ignored here -- otherwise pressing a right-hand key
 * would light an unrelated LED on the left.
 *
 * The position -> LED index tables were measured on hardware, not derived from
 * the KLOR_LEDorder diagram. Both halves turned out to chain identically in
 * mirrored coordinates, and every one of the 21 switches per half maps to
 * exactly one LED. The encoder push switches (positions 28 and 29) have no LED
 * and are absent from both tables.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_REGISTER(klor_rgb_reactive, LOG_LEVEL_INF);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_LEN  DT_PROP(STRIP_NODE, chain_length)

#define IDLE_LEVEL   CONFIG_KLOR_RGB_REACTIVE_IDLE_BRT
#define PRESS_LEVEL  CONFIG_KLOR_RGB_REACTIVE_PRESS_BRT

/* White, so all three channels carry the same value. 255 * pct / 100. */
#define LEVEL(pct) ((uint8_t)((255 * (pct)) / 100))

#define NO_LED 0xFF

/*
 * position -> LED index, measured 2026-08-16.
 *
 * Indices not listed are NO_LED: on the left that is everything on the right
 * half plus the encoder pushes, and vice versa. A sparse 44-entry table costs
 * 44 bytes and avoids any arithmetic at event time.
 */
#if IS_ENABLED(CONFIG_SHIELD_KLOR_LEFT)
static const uint8_t pos_to_led[] = {
    /*  0 Q */ 18, /*  1 W */ 13, /*  2 E */ 12, /*  3 R */ 6,  /*  4 T */ 5,
    /*  5..9   right half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /* 10 ESC*/ 19, /* 11 A */ 17, /* 12 S */ 14, /* 13 D */ 11, /* 14 F */ 7,
    /* 15 G */ 4,
    /* 16..21  right half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /* 22 SFT*/ 20, /* 23 Z */ 16, /* 24 X */ 15, /* 25 C */ 10, /* 26 V */ 8,
    /* 27 B */ 3,
    /* 28 encoder push -- no LED */ NO_LED,
    /* 29 right encoder            */ NO_LED,
    /* 30..35  right half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /* 36 CTL*/ 9,  /* 37 ALT*/ 2,  /* 38 XTRA*/ 1, /* 39 SPC*/ 0,
    /* 40..43  right half */ NO_LED, NO_LED, NO_LED, NO_LED,
};
#else /* SHIELD_KLOR_RIGHT */
static const uint8_t pos_to_led[] = {
    /*  0..4    left half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /*  5 Y */ 5,  /*  6 U */ 6,  /*  7 I */ 12, /*  8 O */ 13, /*  9 P */ 18,
    /* 10..15   left half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /* 16 H */ 4,  /* 17 J */ 7,  /* 18 K */ 11, /* 19 L */ 14, /* 20 ] */ 17,
    /* 21 BSP*/ 19,
    /* 22..27   left half */ NO_LED, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED,
    /* 28 left encoder  */ NO_LED,
    /* 29 encoder push  */ NO_LED,
    /* 30 N */ 3,  /* 31 M */ 8,  /* 32 [ */ 10, /* 33 ; */ 15, /* 34 \ */ 16,
    /* 35 ENT*/ 20,
    /* 36..39   left half */ NO_LED, NO_LED, NO_LED, NO_LED,
    /* 40 , */ 0,  /* 41 . */ 1,  /* 42 / */ 2,  /* 43 ' */ 9,
};
#endif

#define POS_COUNT ((int)ARRAY_SIZE(pos_to_led))

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_LEN];

/* How many keys are currently holding each LED bright. Counting rather than a
 * flag keeps things correct if a key repeats or two positions ever share an
 * LED, and makes release order irrelevant. */
static uint8_t held[STRIP_LEN];

static void render(struct k_work *work) {
    for (int i = 0; i < STRIP_LEN; i++) {
        uint8_t v = held[i] ? LEVEL(PRESS_LEVEL) : LEVEL(IDLE_LEVEL);
        pixels[i].r = pixels[i].g = pixels[i].b = v;
    }

    int rc = led_strip_update_rgb(strip, pixels, STRIP_LEN);
    if (rc) {
        LOG_ERR("led_strip_update_rgb failed: %d", rc);
    }
}

/* The SPI write takes a moment and must not run in the event callback, which
 * is on the caller's thread. Bouncing through the system work queue also
 * naturally coalesces bursts of key events into one update. */
static K_WORK_DEFINE(render_work, render);

static int on_position(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Only this board's own keys. The central also sees the peripheral's, and
     * acting on those would light the wrong strip. */
    if (ev->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position >= POS_COUNT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t led = pos_to_led[ev->position];
    if (led == NO_LED || led >= STRIP_LEN) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        held[led]++;
    } else if (held[led]) {
        held[led]--;
    }

    k_work_submit(&render_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(klor_rgb_reactive, on_position);
ZMK_SUBSCRIPTION(klor_rgb_reactive, zmk_position_state_changed);

static int klor_rgb_reactive_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("led strip not ready -- reactive underglow disabled");
        return -ENODEV;
    }

    memset(held, 0, sizeof(held));
    LOG_INF("reactive underglow: %d LEDs, idle %d%%, press %d%%", STRIP_LEN, IDLE_LEVEL,
            PRESS_LEVEL);

    /* Paint the idle level once so the board comes up lit rather than dark. */
    k_work_submit(&render_work);
    return 0;
}

SYS_INIT(klor_rgb_reactive_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

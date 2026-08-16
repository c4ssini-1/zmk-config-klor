/*
 * KLOR LED mapping harness -- TEMPORARY, delete once the map is known.
 *
 * Walks a single lit LED along the strip so the physical key sitting over each
 * chain index can be identified. This exists because per-key reactive lighting
 * needs a key-position -> LED-index table, and that table cannot be derived
 * reliably from the KLOR_LEDorder diagram alone.
 *
 * Sequence, repeating forever:
 *
 *   1. every LED dim white for 700 ms   <- marker, means "index 0 is next"
 *   2. all off for 300 ms
 *   3. index 0 lit for 3 s, then index 1, ... up to chain-length - 1
 *
 * So the run always starts right after the flash. Watch the flash, then read
 * off the key under each LED in turn.
 *
 * This owns the strip outright, which is why CONFIG_ZMK_RGB_UNDERGLOW must be
 * n while it is in use -- two writers on one led_strip device would fight.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(klor_led_map, LOG_LEVEL_INF);

#define STRIP_NODE  DT_CHOSEN(zmk_underglow)
#define STRIP_LEN   DT_PROP(STRIP_NODE, chain_length)

/* Deliberately dim. White on every LED at high brightness browns out the rail
 * on this board; one LED at a time is nothing, but keep it consistent. */
#define LIT     ((struct led_rgb){.r = 60, .g = 60, .b = 60})
#define MARKER  ((struct led_rgb){.r = 12, .g = 12, .b = 12})

#define STEP_MS    3000
#define MARKER_MS   700
#define GAP_MS      300

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_LEN];

/* -2 = marker frame, -1 = blank gap, 0..STRIP_LEN-1 = that index lit */
static int phase = -2;

static void advance(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step_work, advance);

static void advance(struct k_work *work) {
    uint32_t next_ms;

    memset(pixels, 0, sizeof(pixels));

    if (phase == -2) {
        for (int i = 0; i < STRIP_LEN; i++) {
            pixels[i] = MARKER;
        }
        next_ms = MARKER_MS;
        LOG_INF("LED MAP: marker -- index 0 comes next");
    } else if (phase == -1) {
        next_ms = GAP_MS;
    } else {
        pixels[phase] = LIT;
        next_ms = STEP_MS;
        LOG_INF("LED MAP: index %d of %d lit", phase, STRIP_LEN - 1);
    }

    int rc = led_strip_update_rgb(strip, pixels, STRIP_LEN);
    if (rc) {
        LOG_ERR("led_strip_update_rgb failed: %d", rc);
    }

    if (++phase >= STRIP_LEN) {
        phase = -2;
    }

    k_work_reschedule(&step_work, K_MSEC(next_ms));
}

static int klor_led_map_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("led strip device not ready -- harness disabled");
        return -ENODEV;
    }

    LOG_INF("LED MAP harness active, %d LEDs", STRIP_LEN);
    /* Let the rail and the rest of ZMK settle before driving the strip. */
    k_work_reschedule(&step_work, K_MSEC(2000));
    return 0;
}

SYS_INIT(klor_led_map_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

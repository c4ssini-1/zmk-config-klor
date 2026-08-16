/*
 * Custom OLED status screen: a miniature map of the active layer.
 *
 * Replaces ZMK's built-in screen entirely (CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM),
 * so the layer name, battery, output and profile widgets are gone. That is the
 * trade: 128x64 at one bit per pixel has no room for both a keymap and status
 * icons, and the keymap is the thing worth looking at.
 *
 * LEFT HALF ONLY, and that is not a choice. The peripheral never runs the keymap
 * -- it forwards key positions and nothing else -- so zmk_keymap_highest_layer_active()
 * has nothing to report there. ZMK encodes the same restriction in Kconfig:
 * ZMK_WIDGET_LAYER_STATUS depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL.
 *
 * WHY ONE LABEL RATHER THAN 44 OBJECTS
 * Forty-four LVGL objects would cost far more RAM than this needs and would have
 * to be laid out by hand. Instead the whole board is one multi-line string in a
 * single label, using unscii_8 -- an 8x8 fixed-width bitmap font, already enabled
 * in this build. Monospace is what makes the grid line up: 12 columns x 8 px is
 * 96 px wide and 4 rows x 8 px is 32 px tall, so it sits comfortably inside
 * 128x64 with room for line spacing.
 *
 * The glyphs come from src/keymap_glyphs.h, generated from the keymap by
 * scripts/gen_keymap_glyphs.py. Regenerate it after changing the keymap or the
 * display will quietly lie about what the keys do.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "keymap_glyphs.h"

LOG_MODULE_REGISTER(klor_status, LOG_LEVEL_INF);

/* Rows joined by '\n', so ROWS * (COLS + 1) covers the separators, plus a NUL. */
#define BUF_LEN (KEYMAP_GLYPH_ROWS * (KEYMAP_GLYPH_COLS + 1) + 1)

struct layer_state {
    uint8_t layer;
};

static lv_obj_t *keymap_label;
static char keymap_text[BUF_LEN];

static void render_layer(uint8_t layer) {
    if (layer >= KEYMAP_GLYPH_LAYERS) {
        layer = 0;
    }

    char *w = keymap_text;
    for (int r = 0; r < KEYMAP_GLYPH_ROWS; r++) {
        const char *row = keymap_glyphs[layer][r];
        while (*row) {
            *w++ = *row++;
        }
        if (r < KEYMAP_GLYPH_ROWS - 1) {
            *w++ = '\n';
        }
    }
    *w = '\0';

    if (keymap_label) {
        lv_label_set_text(keymap_label, keymap_text);
    }
}

static void set_layer_cb(struct layer_state state) { render_layer(state.layer); }

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    return (struct layer_state){.layer = zmk_keymap_highest_layer_active()};
}

ZMK_DISPLAY_WIDGET_LISTENER(klor_layer_widget, struct layer_state, set_layer_cb,
                            layer_get_state)
ZMK_SUBSCRIPTION(klor_layer_widget, zmk_layer_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen) {
        /* LVGL is out of heap. Returning NULL leaves the display blank, which is
         * survivable; carrying on would dereference NULL and fault the central,
         * taking the split link down with it. */
        LOG_ERR("no LVGL memory for the status screen - check LV_Z_MEM_POOL_SIZE");
        return NULL;
    }

    keymap_label = lv_label_create(screen);
    if (!keymap_label) {
        LOG_ERR("no LVGL memory for the keymap label");
        return screen;
    }

    lv_obj_set_style_text_font(keymap_label, &lv_font_unscii_8, LV_PART_MAIN);

    /* 4 rows of 8 px is only 32 of the 64 available, so spread them out --
     * easier to read, and it keeps the rows visually distinct on a mono panel. */
    lv_obj_set_style_text_line_space(keymap_label, 6, LV_PART_MAIN);
    lv_obj_set_style_text_align(keymap_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(keymap_label, LV_ALIGN_CENTER, 0, 0);

    /* Paint immediately: layer_state_changed only fires on a change, so without
     * this the screen stays blank until the first layer switch. */
    render_layer(zmk_keymap_highest_layer_active());

    klor_layer_widget_init();
    return screen;
}

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
 * ONLY THIS HALF'S KEYS ARE DRAWN. The full 44-key grid fitted, but reading it
 * meant picking your own hand out of a dense block, and half the screen showed
 * keys under the other hand. Columns 0-5 are the left half; the right half's
 * columns are simply not rendered. That frees enough width to space the glyphs
 * out across the whole panel instead of cramming them into the middle.
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

/* Columns 0..5 are this half. The transform is 12 wide, split evenly. */
#define HALF_COLS (KEYMAP_GLYPH_COLS / 2)

/* Rows joined by '\n', so OUT_ROWS * (OUT_COLS + 1) covers the separators, plus NUL. */
#define BUF_LEN (OUT_ROWS * (OUT_COLS + 1) + 1)

/*
 * A dot is interleaved between every pair of keys ACROSS a row, so the output grid
 * is twice the key columns minus one:
 *
 *     Q . W . E . R . T
 *     e . A . S . D . F
 *
 * Dots share the baseline with the letters, which is the whole point. An
 * earlier version put them on their own rows to make a full lattice, and it
 * looked wrong: '.' in unscii_8 is a 2x2 glyph sitting on the baseline, so a
 * dedicated dot row is 7 of its 9 px empty and the dot hugs the row below
 * instead of sitting midway between two.
 *
 * SIZING. Two constants here are easy to get wrong and both have already
 * caused visible bugs:
 *
 *   - unscii_8's line_height is 9, not 8. The glyph box is 8x8 but the font
 *     declares 9. Budgeting on 8 overflowed the panel and pushed the bottom
 *     rows off the screen.
 *   - LVGL adds letter_space after EVERY character, not only between them.
 *     Budgeting on gaps-only overflowed the width and drew a scrollbar.
 *
 * So both are budgeted at (size + space) * count:
 *
 *   width  = OUT_COLS * (8 + LETTER_SPACE) = 11 * (8 + 3) = 121 of 128
 *   height = OUT_ROWS * (9 + LINE_SPACE)   =  4 * (9 + 6) =  60 of 64
 */
#define OUT_COLS (2 * HALF_COLS - 1)
#define OUT_ROWS (KEYMAP_GLYPH_ROWS)

#define LETTER_SPACE 3
#define LINE_SPACE   6

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
    for (int R = 0; R < OUT_ROWS; R++) {
        const char *row = keymap_glyphs[layer][R];
        for (int C = 0; C < OUT_COLS; C++) {
            if (C % 2 == 0) {
                *w++ = row[C / 2];
            } else {
                /* A dot only where it genuinely sits between two keys. Rows 0
                 * and 3 have no column 0 or 11, so without this an orphan dot
                 * floats at the edge with nothing beside it.
                 *
                 * Test presence, not the glyph: Space is a real key drawn as
                 * blank, and its neighbouring dots must stay. */
                const char *pres = keymap_present[R];
                *w++ = (pres[(C - 1) / 2] != ' ' && pres[(C + 1) / 2] != ' ') ? '.' : ' ';
            }
        }
        if (R < OUT_ROWS - 1) {
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

    /* Belt and braces against the scrollbar: even if the text is ever a pixel
     * too wide, no bar should appear over the keymap. */
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    keymap_label = lv_label_create(screen);
    if (!keymap_label) {
        LOG_ERR("no LVGL memory for the keymap label");
        return screen;
    }

    lv_obj_set_style_text_font(keymap_label, &lv_font_unscii_8, LV_PART_MAIN);

    /* Spread to fill the panel rather than clustering in the centre. */
    lv_obj_set_style_text_letter_space(keymap_label, LETTER_SPACE, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(keymap_label, LINE_SPACE, LV_PART_MAIN);
    lv_obj_set_style_text_align(keymap_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_clear_flag(keymap_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(keymap_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(keymap_label, LV_ALIGN_CENTER, 0, 0);

    /* Paint immediately: layer_state_changed only fires on a change, so without
     * this the screen stays blank until the first layer switch. */
    render_layer(zmk_keymap_highest_layer_active());

    klor_layer_widget_init();
    return screen;
}

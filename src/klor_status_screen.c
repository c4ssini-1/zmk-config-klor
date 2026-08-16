/*
 * Custom OLED status screen: a miniature map of the active layer, with the
 * key you are pressing shown inverted.
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
 * The glyphs come from src/keymap_glyphs.h, generated from the keymap by
 * scripts/gen_keymap_glyphs.py. Regenerate it after changing the keymap or the
 * display will quietly lie about what the keys do.
 *
 *
 * WHY A CANVAS RATHER THAN LABELS
 *
 * An LVGL label has no per-character styling, so a plain label cannot invert one
 * glyph. The alternatives were an overlay object per pressed key, or one label
 * per key with its style swapped; both work, but both cap or cost something.
 * Drawing into a canvas means every pixel is ours: the inverted cell is just a
 * white rectangle with the glyph repainted in black over it, and any number of
 * keys can be lit at once because each is simply two more draw calls.
 *
 * The canvas is LV_COLOR_FORMAT_I1 -- one bit per pixel, matching the panel.
 * L8 would be simpler (no palette) but costs 8 KB against I1's 1 KB, and
 * LV_DRAW_SW_SUPPORT_I1 is already enabled in this build so the software
 * renderer can blend into it.
 *
 * TWO BUFFER FACTS THAT WILL BITE IF FORGOTTEN:
 *
 *   - An indexed draw buffer stores its palette in the FIRST bytes of the
 *     buffer, and the pixel data starts after it (lv_draw_buf_goto_xy adds
 *     LV_COLOR_INDEXED_PALETTE_SIZE * 4). lv_canvas_set_buffer does NOT account
 *     for this -- it passes data_size = stride * h -- so the buffer we hand it
 *     must be palette + pixels or LVGL writes off the end of it.
 *   - The palette must be set explicitly. Index 0 is the background and index 1
 *     the foreground; blending picks the index by luminance threshold, so
 *     lv_color_white() lands on 1 and lv_color_black() on 0.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#include "keymap_glyphs.h"

LOG_MODULE_REGISTER(klor_status, LOG_LEVEL_INF);

/* Columns 0..5 are this half. The transform is 12 wide, split evenly. */
#define HALF_COLS (KEYMAP_GLYPH_COLS / 2)

/*
 * A dot is interleaved between every pair of keys ACROSS a row, so the output
 * grid is twice the key columns minus one:
 *
 *     Q . W . E . R . T
 *     e . A . S . D . F
 *
 * Dots share the baseline with the letters, which is the whole point. An
 * earlier version put them on their own rows to make a full lattice, and it
 * looked wrong: '.' in unscii_8 is a 2x2 glyph sitting on the baseline, so a
 * dedicated dot row is 7 of its 9 px empty and the dot hugs the row below
 * instead of sitting midway between two.
 */
#define OUT_COLS (2 * HALF_COLS - 1)
#define OUT_ROWS (KEYMAP_GLYPH_ROWS)

/* Rows joined by '\n', so OUT_ROWS * (OUT_COLS + 1) covers the separators, plus NUL. */
#define BUF_LEN (OUT_ROWS * (OUT_COLS + 1) + 1)

/*
 * SIZING. These constants are easy to get wrong and both have already caused
 * visible bugs:
 *
 *   - unscii_8's line_height is 9, not 8. The glyph box is 8x8 but the font
 *     declares 9. Budgeting on 8 overflowed the panel and pushed the bottom
 *     rows off the screen.
 *   - LVGL advances by (glyph width + letter_space) after EVERY character, not
 *     only between them. Budgeting on gaps-only overflowed the width.
 *
 * So a cell is the advance, not the glyph:
 *
 *   width  = OUT_COLS * CELL_W = 11 * 11 = 121 of 128
 *   height = OUT_ROWS * CELL_H =  4 * 15 =  60 of 64
 */
#define GLYPH_W 8
#define GLYPH_H 9

#define LETTER_SPACE 3
#define LINE_SPACE   6

#define CELL_W (GLYPH_W + LETTER_SPACE)
#define CELL_H (GLYPH_H + LINE_SPACE)

#define TEXT_W (OUT_COLS * CELL_W)
#define TEXT_H (OUT_ROWS * CELL_H)

#define CANVAS_W 128
#define CANVAS_H 64

/* Centre the block in the leftover 7x4 px. */
#define ORIGIN_X ((CANVAS_W - TEXT_W) / 2)
#define ORIGIN_Y ((CANVAS_H - TEXT_H) / 2)

/*
 * The pressed-key highlight: a square, sized to sit inside the lattice without
 * touching the dots either side. The glyph is 8 wide by 9 tall, so 11x11 clears
 * it by a pixel all round; the neighbouring dot is a full cell (11 px) away and
 * its ink sits near the left of its own box, so nothing collides.
 */
#define HL_SIDE  11
#define HL_PAD_X ((HL_SIDE - GLYPH_W) / 2)
#define HL_PAD_Y ((HL_SIDE - GLYPH_H) / 2)

/*
 * THE PANEL RUNS INVERTED, SO THESE TWO NAMES ARE THE OPPOSITE OF THE LVGL
 * COLOUR THEY WRAP.
 *
 * klor_common.dtsi declares the SSD1306 with "inversion-on", so a pixel LVGL
 * considers white arrives on the glass dark, and vice versa. That node is
 * shared with the right half, whose built-in status screen is already correct
 * against it, so the inversion is absorbed here rather than changed there.
 *
 * CONFIG_ZMK_DISPLAY_INVERT is not the lever either. It only passes a
 * dark-background flag to lv_theme_mono_init, which styles widgets; a canvas
 * paints its own pixels and never consults the theme.
 *
 * Naming these for the result on the glass keeps the drawing code below
 * readable -- it says "black background, white text" and that is what you see.
 */
#define ON_GLASS_WHITE lv_color_black()
#define ON_GLASS_BLACK lv_color_white()

/*
 * Canvas backing store. Palette first, then pixels -- see the header comment.
 * 4-byte aligned because the palette entries are lv_color32_t.
 */
#define CANVAS_STRIDE ((CANVAS_W + 7) / 8)
#define I1_PALETTE_BYTES (2 * sizeof(lv_color32_t))

static uint8_t canvas_buf[I1_PALETTE_BYTES + CANVAS_STRIDE * CANVAS_H] __aligned(4);

struct layer_state {
    uint8_t layer;
};

struct key_state {
    /* One bit per key position. 44 positions fit a uint64_t with room over. */
    uint64_t held;
};

static lv_obj_t *keymap_canvas;

static uint8_t cur_layer;
static uint64_t cur_held;

static char keymap_text[BUF_LEN];

/*
 * One NUL-terminated single-character string per position, for the black glyph
 * painted over a highlight. lv_draw_label with text_local = 0 keeps the caller's
 * pointer and reads it when the task is dispatched, so these must outlive the
 * draw calls -- a stack buffer reused round the loop would be a dangling read.
 */
static char hl_text[KEYMAP_GLYPH_POSITIONS][2];

static void build_text(uint8_t layer) {
    char *w = keymap_text;
    for (int R = 0; R < OUT_ROWS; R++) {
        const char *row = keymap_glyphs[layer][R];
        for (int C = 0; C < OUT_COLS; C++) {
            /* Keys on even columns, a lattice dot on every odd one -- including
             * where the neighbouring cell has no key. Rows 0 and 3 have no
             * column 0, and suppressing the dot there left those rows starting
             * flush while the others started with a dot, which broke the grid.
             * A continuous dot column reads as a grid; a ragged one does not.
             */
            *w++ = (C % 2 == 0) ? row[C / 2] : '.';
        }
        if (R < OUT_ROWS - 1) {
            *w++ = '\n';
        }
    }
    *w = '\0';
}

static void render(void) {
    if (!keymap_canvas) {
        return;
    }

    uint8_t layer = cur_layer < KEYMAP_GLYPH_LAYERS ? cur_layer : 0;
    uint64_t held = cur_held;

    build_text(layer);

    lv_layer_t l;
    lv_canvas_init_layer(keymap_canvas, &l);

    /* Blank the panel. A full-canvas rect goes through the same draw path as
     * everything else; lv_canvas_fill_bg would fall back to a per-pixel loop
     * for indexed formats. */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = ON_GLASS_BLACK;
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = {0, 0, CANVAS_W - 1, CANVAS_H - 1};
    lv_draw_rect(&l, &bg, &full);

    /* The keymap itself, white on black. */
    lv_draw_label_dsc_t txt;
    lv_draw_label_dsc_init(&txt);
    txt.text = keymap_text;
    txt.font = &lv_font_unscii_8;
    txt.color = ON_GLASS_WHITE;
    txt.letter_space = LETTER_SPACE;
    txt.line_space = LINE_SPACE;
    txt.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t text_area = {ORIGIN_X, ORIGIN_Y, ORIGIN_X + TEXT_W - 1, ORIGIN_Y + TEXT_H - 1};
    lv_draw_label(&l, &txt, &text_area);

    /* Then invert every held key on this half. Draw tasks run in the order they
     * are added, so the white square lands on top of the white glyph drawn
     * above and the black glyph lands on top of the square. */
    for (int p = 0; p < KEYMAP_GLYPH_POSITIONS; p++) {
        if (!(held & (1ULL << p))) {
            continue;
        }

        int r = keymap_pos_row[p];
        int c = keymap_pos_col[p];

        /* The right half is not drawn, so its keys have nowhere to light up.
         * Equivalent to filtering on ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL --
         * this half owns exactly columns 0..5 -- but tied to what is on screen
         * rather than to which board sent the event. */
        if (c >= HALF_COLS) {
            continue;
        }

        int gx = ORIGIN_X + (2 * c) * CELL_W;
        int gy = ORIGIN_Y + r * CELL_H;

        lv_draw_rect_dsc_t sq;
        lv_draw_rect_dsc_init(&sq);
        sq.bg_color = ON_GLASS_WHITE;
        sq.bg_opa = LV_OPA_COVER;
        lv_area_t sq_area = {
            gx - HL_PAD_X,
            gy - HL_PAD_Y,
            gx - HL_PAD_X + HL_SIDE - 1,
            gy - HL_PAD_Y + HL_SIDE - 1,
        };
        lv_draw_rect(&l, &sq, &sq_area);

        hl_text[p][0] = keymap_glyphs[layer][r][c];
        hl_text[p][1] = '\0';

        lv_draw_label_dsc_t glyph;
        lv_draw_label_dsc_init(&glyph);
        glyph.text = hl_text[p];
        glyph.font = &lv_font_unscii_8;
        glyph.color = ON_GLASS_BLACK;
        glyph.align = LV_TEXT_ALIGN_LEFT;
        lv_area_t glyph_area = {gx, gy, gx + GLYPH_W - 1, gy + GLYPH_H - 1};
        lv_draw_label(&l, &glyph, &glyph_area);
    }

    lv_canvas_finish_layer(keymap_canvas, &l);
}

static void set_layer_cb(struct layer_state state) {
    /* Same reasoning as set_key_cb: zmk_layer_state_changed fires on activate
     * and deactivate both, and the highest active layer is often the same
     * afterwards. Repainting then costs a full panel flush for nothing. */
    if (state.layer == cur_layer) {
        return;
    }
    cur_layer = state.layer;
    render();
}

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    return (struct layer_state){.layer = zmk_keymap_highest_layer_active()};
}

ZMK_DISPLAY_WIDGET_LISTENER(klor_layer_widget, struct layer_state, set_layer_cb,
                            layer_get_state)
ZMK_SUBSCRIPTION(klor_layer_widget, zmk_layer_state_changed);

/*
 * The held set is accumulated HERE, in the state fetch, not in the display
 * callback. ZMK_DISPLAY_WIDGET_LISTENER stores one state snapshot and submits a
 * k_work; if a second key event arrives before that work runs, the submit is a
 * no-op and only the newer snapshot survives. A state of "which key just
 * changed" would therefore drop events and leave highlights stuck on. Carrying
 * the whole held set means a coalesced update is still correct -- it just
 * redraws once instead of twice.
 *
 * Runs under the listener's mutex, which is the only writer, so held_acc needs
 * no further locking.
 */
static uint64_t held_acc;

static void set_key_cb(struct key_state state) {
    /* A redraw is a full canvas repaint and a full-panel I2C flush, so it must
     * not happen unless the picture actually changes. Without this the screen
     * repainted identically on every key the grid does not show. */
    if (state.held == cur_held) {
        return;
    }
    cur_held = state.held;
    render();
}

static struct key_state key_get_state(const zmk_event_t *eh) {
    /* eh is NULL on the initial call from klor_key_widget_init(). */
    const struct zmk_position_state_changed *ev =
        eh ? as_zmk_position_state_changed(eh) : NULL;

    /* Only positions this half draws are tracked. The central also sees the
     * peripheral's keys, and those live in columns 6..11, which are not
     * rendered -- letting them into the held set would mean a full repaint for
     * every right-hand keystroke that changed nothing on screen. */
    if (ev && ev->position < KEYMAP_GLYPH_POSITIONS &&
        keymap_pos_col[ev->position] < HALF_COLS) {
        if (ev->state) {
            held_acc |= 1ULL << ev->position;
        } else {
            held_acc &= ~(1ULL << ev->position);
        }
    }

    return (struct key_state){.held = held_acc};
}

ZMK_DISPLAY_WIDGET_LISTENER(klor_key_widget, struct key_state, set_key_cb, key_get_state)
ZMK_SUBSCRIPTION(klor_key_widget, zmk_position_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen) {
        /* LVGL is out of heap. Returning NULL leaves the display blank, which is
         * survivable; carrying on would dereference NULL and fault the central,
         * taking the split link down with it. */
        LOG_ERR("no LVGL memory for the status screen - check LV_Z_MEM_POOL_SIZE");
        return NULL;
    }

    /* The canvas is exactly panel-sized, so any padding or border on the screen
     * would push it out of view. */
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    keymap_canvas = lv_canvas_create(screen);
    if (!keymap_canvas) {
        LOG_ERR("no LVGL memory for the keymap canvas");
        return screen;
    }

    lv_canvas_set_buffer(keymap_canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(keymap_canvas, 0,
                          (lv_color32_t){.red = 0, .green = 0, .blue = 0, .alpha = 255});
    lv_canvas_set_palette(keymap_canvas, 1,
                          (lv_color32_t){.red = 255, .green = 255, .blue = 255, .alpha = 255});

    lv_obj_set_size(keymap_canvas, CANVAS_W, CANVAS_H);
    lv_obj_align(keymap_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Paint immediately: both events only fire on a change, so without this the
     * screen stays blank until the first keypress or layer switch. */
    cur_layer = zmk_keymap_highest_layer_active();
    cur_held = 0;
    render();

    klor_layer_widget_init();
    klor_key_widget_init();
    return screen;
}

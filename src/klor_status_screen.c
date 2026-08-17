/*
 * Custom OLED status screen: a miniature map of the active layer, with the
 * key you are pressing shown inverted.
 *
 * Replaces ZMK's built-in screen entirely (CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM),
 * so the layer name, battery, output and profile widgets are gone. That is the
 * trade: 128x64 at one bit per pixel has no room for both a keymap and status
 * icons, and the keymap is the thing worth looking at.
 *
 * RUNS ON BOTH HALVES, each drawing only its own keys. The full 44-key grid
 * fitted, but reading it meant picking your own hand out of a dense block, and
 * half the screen showed keys under the other hand. Columns 0-5 are the left
 * half and 6-11 the right; each board renders its own six and ignores the rest.
 * That frees enough width to space the glyphs out across the whole panel
 * instead of cramming them into the middle.
 *
 * Pressed-key highlighting works identically on both, because
 * zmk_position_state_changed is raised locally by each board's own matrix scan.
 * Nothing crosses the split link for it.
 *
 * THE LAYER IS CENTRAL-ONLY, AND THAT IS A HARD BUILD CONSTRAINT, not a choice.
 * The peripheral never runs the keymap, and ZMK does not even compile src/keymap.c
 * or src/events/layer_state_changed.c into a non-central build -- see the
 * "if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)" block in
 * app/CMakeLists.txt. So zmk_keymap_highest_layer_active() and a subscription to
 * the layer event would both fail to LINK on the right half, not merely return
 * nothing. Everything layer-related is therefore compiled out there, and the
 * peripheral draws BASE until the layer is carried across the split link.
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
#include <zmk/events/position_state_changed.h>

/*
 * Same condition ZMK uses to decide whether to compile keymap.c and
 * layer_state_changed.c at all. Guarding on it keeps the peripheral from
 * referencing symbols that were never built.
 */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define KLOR_HAS_LAYER_STATE 1
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#else
#define KLOR_HAS_LAYER_STATE 0
#endif

/*
 * The split-link indicator is the mirror image: only the peripheral can report
 * whether it has found the other half. zmk_split_peripheral_status_changed is
 * raised by ZMK's own peripheral.c on connect and disconnect, and
 * bluetooth/peripheral.c -- which provides the getter for the state at boot --
 * is compiled only when NOT ZMK_SPLIT_ROLE_CENTRAL.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define KLOR_SHOW_LINK_STATUS 1
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#else
#define KLOR_SHOW_LINK_STATUS 0
#endif

/* Guarded on the Kconfig symbol, not KLOR_SHOW_BATTERY: that is defined further
 * down with the geometry, so testing it here would silently skip both includes
 * and leave the calls below as implicit declarations. */
#if IS_ENABLED(CONFIG_KLOR_BATTERY_BAR)
#include <zmk/battery.h>
/*
 * VBUS is read straight from the nRF52840 POWER peripheral rather than through
 * ZMK's USB API, and that is not a shortcut -- ZMK_USB is declared
 * "depends on (!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)", so the peripheral cannot
 * have it at all. This register works the same on both halves.
 */
#include <hal/nrf_power.h>
#endif

#include "keymap_glyphs.h"

LOG_MODULE_REGISTER(klor_status, LOG_LEVEL_INF);

/* The transform is 12 columns wide, split evenly between the two halves. */
#define HALF_COLS (KEYMAP_GLYPH_COLS / 2)

/*
 * Which slice of the grid this board draws. The left overlay owns columns 0-5;
 * the right adds col-offset = <6> so its keys land in 6-11.
 */
#if IS_ENABLED(CONFIG_SHIELD_KLOR_LEFT)
#define COL_BASE 0
#else
#define COL_BASE HALF_COLS
#endif

/* Grid column -> cell index on this panel, or -1 for the other half's keys. */
static inline int local_col(int c) {
    int lc = c - COL_BASE;
    return (lc >= 0 && lc < HALF_COLS) ? lc : -1;
}

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

/*
 * BATTERY BAR. A 2 px rule along the very top, its length the charge level.
 * Each half shows its own cell -- the battery is local hardware and
 * zmk_battery_state_of_charge() reads it on both boards.
 *
 * Blinking at 1 Hz means charging. Solid means not charging, which covers both
 * "on battery" and "finished" -- the bar's own length tells those apart.
 *
 * WHAT "FINISHED" ACTUALLY MEANS HERE. The nice!nano v2 exposes no charge
 * status to the MCU: its board overlay declares vbatt (zmk,battery-nrf-vddh)
 * and an EXT_POWER pin, and nothing else. So "charging" is inferred as
 * "USB present and not yet at KLOR_BATTERY_FULL_PCT". That reads optimistically
 * on purpose-built hardware and doubly so here, because VDDH carries the
 * charger's output while plugged in rather than the resting cell voltage -- it
 * will call the battery full before it is. Treat the solid bar as "topping
 * off", not "done".
 */
#if IS_ENABLED(CONFIG_KLOR_BATTERY_BAR)
#define KLOR_SHOW_BATTERY 1
#define BATT_BAR_H 2
/* The bar plus one blank pixel, so it never touches the top row of glyphs. */
#define TOP_RESERVED (BATT_BAR_H + 1)
#else
#define KLOR_SHOW_BATTERY 0
#define TOP_RESERVED 0
#endif

/* Centre the block in what is left once the bar has taken its strip. */
#define ORIGIN_X ((CANVAS_W - TEXT_W) / 2)
#define ORIGIN_Y (TOP_RESERVED + ((CANVAS_H - TOP_RESERVED - TEXT_H) / 2))

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

#if KLOR_SHOW_LINK_STATUS
/*
 * A tick when this half has found the central, a cross when it has not, drawn
 * in the last cell of the bottom row.
 *
 * THAT CELL IS EMPTY BY LUCK OF THE MATRIX, NOT BY RESERVATION. The transform
 * puts no key at row 3, column 11 -- the bottom row's rightmost position is
 * column 10, the apostrophe -- so the mark has the cell to itself. If a key is
 * ever added there, this will draw over its glyph.
 *
 * unscii_8 is 7-bit ASCII and has neither a tick nor a cross, so these are
 * pixel bitmaps rather than characters. One bit per pixel, bit 8 leftmost --
 * uint16_t rather than uint8_t because the marks are 9 wide. They are painted
 * with lv_canvas_set_px after the draw layer is dispatched, which keeps them
 * off the draw-task heap entirely; for indexed formats that call takes the
 * palette index straight from color.blue, so the same ON_GLASS_* colours apply.
 *
 * STROKES ARE 2 px. A single-pixel stroke was legible only if you went looking
 * for it -- on a 128x64 panel these are about a millimetre across. The marks
 * are sized to the 11x11 highlight square they sit in rather than to the 8x9
 * glyph box, which is what buys the room for the thicker stroke.
 */
#define MARK_W 9
#define MARK_H 9

static const uint16_t mark_tick[MARK_H] = {
    0x000, /* ......... */
    0x001, /* ........# */
    0x003, /* .......## */
    0x106, /* #.....##. */
    0x18c, /* ##...##.. */
    0x0d8, /* .##.##... */
    0x070, /* ..###.... */
    0x020, /* ...#..... */
    0x000, /* ......... */
};

static const uint16_t mark_cross[MARK_H] = {
    0x101, /* #.......# */
    0x183, /* ##.....## */
    0x0c6, /* .##...##. */
    0x06c, /* ..##.##.. */
    0x038, /* ...###... */
    0x06c, /* ..##.##.. */
    0x0c6, /* .##...##. */
    0x183, /* ##.....## */
    0x101, /* #.......# */
};

/* Bottom row, rightmost cell -- immediately after the apostrophe. */
#define LINK_CELL_ROW (OUT_ROWS - 1)
#define LINK_CELL_COL (HALF_COLS - 1)
#endif /* KLOR_SHOW_LINK_STATUS */

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
            *w++ = (C % 2 == 0) ? row[COL_BASE + C / 2] : '.';
        }
        if (R < OUT_ROWS - 1) {
            *w++ = '\n';
        }
    }
    *w = '\0';
}

#if KLOR_SHOW_LINK_STATUS
static bool link_connected;

static void draw_link_mark(void) {
    const uint16_t *rows = link_connected ? mark_tick : mark_cross;

    /* Centred in the 11x11 highlight square, not in the 8x9 glyph box -- the
     * mark is a badge filling that square, and at 9 px it is wider than a
     * glyph. Squaring it against the box instead would push it off centre and
     * crowd the right edge. */
    int x0 = ORIGIN_X + (2 * LINK_CELL_COL) * CELL_W - HL_PAD_X + (HL_SIDE - MARK_W) / 2;
    int y0 = ORIGIN_Y + LINK_CELL_ROW * CELL_H - HL_PAD_Y + (HL_SIDE - MARK_H) / 2;

    for (int y = 0; y < MARK_H; y++) {
        for (int x = 0; x < MARK_W; x++) {
            if (rows[y] & (1 << (MARK_W - 1 - x))) {
                /* Black, because it sits on the permanent white square that
                 * render() lays down for this cell. */
                lv_canvas_set_px(keymap_canvas, x0 + x, y0 + y, ON_GLASS_BLACK, LV_OPA_COVER);
            }
        }
    }
}
#endif /* KLOR_SHOW_LINK_STATUS */

#if KLOR_SHOW_BATTERY

static uint8_t batt_soc;
static bool batt_charging;
static bool batt_blink_on = true;

static inline bool vbus_present(void) {
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
}

/* Drawn inside the layer, so it is dispatched with everything else. */
static void draw_battery(lv_layer_t *l) {
    if (batt_charging && !batt_blink_on) {
        return; /* the dark half of the blink */
    }

    int w = (batt_soc * CANVAS_W) / 100;
    if (w <= 0) {
        /* Keep one pixel at empty: a bar that vanishes entirely is
         * indistinguishable from the feature being broken. */
        w = 1;
    }

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = ON_GLASS_WHITE;
    bar.bg_opa = LV_OPA_COVER;

    lv_area_t area = {0, 0, w - 1, BATT_BAR_H - 1};
    lv_draw_rect(l, &bar, &area);
}

#endif /* KLOR_SHOW_BATTERY */

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

#if KLOR_SHOW_BATTERY
    draw_battery(&l);
#endif

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

#if KLOR_SHOW_LINK_STATUS
    /* The link mark is permanently inverted, the way a held key looks: the
     * white square goes down here as part of the layer, and draw_link_mark()
     * paints the tick or cross onto it in black once the layer is dispatched.
     * Nothing else claims this cell -- the matrix has no key at row 3,
     * column 11 -- so it never fights with a keypress highlight. */
    {
        int lx = ORIGIN_X + (2 * LINK_CELL_COL) * CELL_W;
        int ly = ORIGIN_Y + LINK_CELL_ROW * CELL_H;

        lv_draw_rect_dsc_t link_sq;
        lv_draw_rect_dsc_init(&link_sq);
        link_sq.bg_color = ON_GLASS_WHITE;
        link_sq.bg_opa = LV_OPA_COVER;

        lv_area_t link_area = {
            lx - HL_PAD_X,
            ly - HL_PAD_Y,
            lx - HL_PAD_X + HL_SIDE - 1,
            ly - HL_PAD_Y + HL_SIDE - 1,
        };
        lv_draw_rect(&l, &link_sq, &link_area);
    }
#endif

    /* Then invert every held key on this half. Draw tasks run in the order they
     * are added, so the white square lands on top of the white glyph drawn
     * above and the black glyph lands on top of the square. */
    for (int p = 0; p < KEYMAP_GLYPH_POSITIONS; p++) {
        if (!(held & (1ULL << p))) {
            continue;
        }

        int r = keymap_pos_row[p];
        int lc = local_col(keymap_pos_col[p]);

        /* The other half's keys are not drawn, so they have nowhere to light
         * up. Equivalent to filtering on ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
         * since each board owns exactly its own six columns, but tied to what is
         * on screen rather than to which board sent the event. */
        if (lc < 0) {
            continue;
        }

        int gx = ORIGIN_X + (2 * lc) * CELL_W;
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

        /* Indexed by the absolute grid column, not the on-panel cell. */
        hl_text[p][0] = keymap_glyphs[layer][r][keymap_pos_col[p]];
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

#if KLOR_SHOW_LINK_STATUS
    /* After the layer is dispatched, or the queued draw tasks would paint over
     * these pixels. */
    draw_link_mark();
#endif
}

#if !KLOR_HAS_LAYER_STATE

/*
 * The peripheral cannot read the keymap, so the central pushes the layer to it
 * over the split link -- see src/klor_layer_sync.c. This is the receiving end.
 *
 * The call arrives on whichever thread handled the split command, never the
 * display thread, so it only records the value and bounces the redraw onto the
 * display work queue. Doing LVGL work directly from here would race the
 * renderer.
 */
static uint8_t pushed_layer;

static void pushed_layer_work_cb(struct k_work *work) {
    if (pushed_layer == cur_layer) {
        return;
    }
    cur_layer = pushed_layer;
    render();
}

static K_WORK_DEFINE(pushed_layer_work, pushed_layer_work_cb);

void klor_status_set_layer(uint8_t layer) {
    pushed_layer = layer;

    /* Before the display is up there is nothing to submit to; the value is
     * still recorded, and the initial paint will pick it up. */
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &pushed_layer_work);
    }
}

#endif /* !KLOR_HAS_LAYER_STATE */

#if KLOR_HAS_LAYER_STATE

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

#endif /* KLOR_HAS_LAYER_STATE */

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

#if KLOR_SHOW_BATTERY

/*
 * One timer drives both jobs, because neither has an event to hang off.
 * zmk_battery_state_changed exists, but VBUS has no event at all on the
 * peripheral, so something has to poll -- and once it is polling, reading the
 * cached charge level in the same pass is free.
 *
 * It runs on the display queue, so it can call render() directly instead of
 * marshalling. Nothing is redrawn unless something actually changed; while
 * charging, the blink phase changes every tick, which is the redraw.
 */
#define BATT_TICK_CHARGING_MS 500 /* two ticks per second = 1 Hz blink */
#define BATT_TICK_IDLE_MS     2000

static void batt_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(batt_work, batt_work_cb);

static void batt_work_cb(struct k_work *work) {
    uint8_t soc = zmk_battery_state_of_charge();
    bool charging = vbus_present() && soc < CONFIG_KLOR_BATTERY_FULL_PCT;

    /* Solid whenever not charging, so a stopped blink always leaves the bar
     * visible rather than possibly stranded in its dark phase. */
    bool blink = charging ? !batt_blink_on : true;

    if (soc != batt_soc || charging != batt_charging || blink != batt_blink_on) {
        batt_soc = soc;
        batt_charging = charging;
        batt_blink_on = blink;
        render();
    }

    k_work_reschedule_for_queue(
        zmk_display_work_q(), &batt_work,
        K_MSEC(charging ? BATT_TICK_CHARGING_MS : BATT_TICK_IDLE_MS));
}

#endif /* KLOR_SHOW_BATTERY */

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

    /* Only positions this board draws are tracked. The central also sees the
     * peripheral's keys, which are not rendered here -- letting them into the
     * held set would mean a full repaint for every keystroke on the other hand
     * that changed nothing on screen. */
    if (ev && ev->position < KEYMAP_GLYPH_POSITIONS &&
        local_col(keymap_pos_col[ev->position]) >= 0) {
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

#if KLOR_SHOW_LINK_STATUS

struct link_state {
    bool connected;
};

static void set_link_cb(struct link_state state) {
    if (state.connected == link_connected) {
        return;
    }
    link_connected = state.connected;
    render();
}

static struct link_state link_get_state(const zmk_event_t *eh) {
    /* eh is NULL on the initial call from klor_link_widget_init(); fall back to
     * asking ZMK directly so a screen that starts up after the halves have
     * already paired shows a tick rather than waiting for a disconnect. */
    const struct zmk_split_peripheral_status_changed *ev =
        eh ? as_zmk_split_peripheral_status_changed(eh) : NULL;

    return (struct link_state){.connected = ev ? ev->connected
                                               : zmk_split_bt_peripheral_is_connected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(klor_link_widget, struct link_state, set_link_cb, link_get_state)
ZMK_SUBSCRIPTION(klor_link_widget, zmk_split_peripheral_status_changed);

#endif /* KLOR_SHOW_LINK_STATUS */

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
#if KLOR_HAS_LAYER_STATE
    cur_layer = zmk_keymap_highest_layer_active();
#else
    /* The peripheral has no keymap to ask, so it shows BASE. */
    cur_layer = 0;
#endif
    cur_held = 0;
#if KLOR_SHOW_LINK_STATUS
    link_connected = zmk_split_bt_peripheral_is_connected();
#endif
#if KLOR_SHOW_BATTERY
    batt_soc = zmk_battery_state_of_charge();
    batt_charging = vbus_present() && batt_soc < CONFIG_KLOR_BATTERY_FULL_PCT;
    batt_blink_on = true;
#endif
    render();

#if KLOR_SHOW_BATTERY
    k_work_reschedule_for_queue(zmk_display_work_q(), &batt_work, K_MSEC(BATT_TICK_IDLE_MS));
#endif

#if KLOR_HAS_LAYER_STATE
    klor_layer_widget_init();
#endif
    klor_key_widget_init();
#if KLOR_SHOW_LINK_STATUS
    klor_link_widget_init();
#endif
    return screen;
}

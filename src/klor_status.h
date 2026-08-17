/*
 * Lets the split layer sync push a layer into the status screen on the
 * peripheral, where the keymap cannot be read.
 */

#pragma once

#include <stdint.h>

/*
 * Peripheral only -- on the central the screen reads the keymap directly and
 * this is neither defined nor needed. Safe to call from any thread; the redraw
 * is marshalled onto the display work queue.
 */
void klor_status_set_layer(uint8_t layer);

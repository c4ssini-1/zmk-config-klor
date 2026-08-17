/*
 * Internal interface between the reactive underglow (klor_rgb_reactive.c) and
 * the behavior that switches it (klor_rgb_behavior.c). Not a public API --
 * these two files are two halves of one feature, split only because a behavior
 * driver needs its own DT_DRV_COMPAT translation unit.
 */

#pragma once

#include <stdbool.h>

/* Both are safe to call before the strip is ready; the state is simply stored
 * and applied on the next render. */
void klor_rgb_set_enabled(bool enabled);
bool klor_rgb_is_enabled(void);

# Sources go into the "app" target rather than a zephyr_library().
#
# ZMK's app/CMakeLists.txt does "target_include_directories(app PRIVATE include)",
# so app/include -- which holds zmk/event_manager.h, zmk/events/* and
# zmk/display.h -- is not exported to modules. A standalone zephyr_library()
# cannot see those headers and fails with "No such file or directory".
#
# Compiling into app inherits its private include paths. This is what the
# established community modules do (zmk-rgbled-widget, zmk-adaptive-key).
target_sources_ifdef(CONFIG_KLOR_RGB_REACTIVE app PRIVATE src/klor_rgb_reactive.c)
target_sources_ifdef(CONFIG_KLOR_RGB_REACTIVE app PRIVATE src/klor_rgb_behavior.c)
target_sources_ifdef(CONFIG_KLOR_STATUS_SCREEN app PRIVATE src/klor_status_screen.c)

# Headers next to the sources: keymap_glyphs.h for the screen, klor_rgb.h shared
# between the underglow and its behavior. The sources are compiled into app from
# a different directory, so the include path needs stating.
if(CONFIG_KLOR_STATUS_SCREEN OR CONFIG_KLOR_RGB_REACTIVE)
  target_include_directories(app PRIVATE src)
endif()

# include/ holds dt-bindings/klor_rgb.h, which BOTH the keymap and the behavior
# source include -- and they reach it by different routes.
#
# The keymap is devicetree, and gets it from zephyr/module.yml's dts_root: that
# adds <root>/include to the devicetree preprocessor's search path. But dts_root
# says nothing to the C compiler, so without the line below the C build fails
# with "dt-bindings/klor_rgb.h: No such file or directory" even though the exact
# same include resolved fine moments earlier while generating the devicetree.
# That is not a hypothetical -- it is how this first landed.
if(CONFIG_KLOR_RGB_REACTIVE)
  target_include_directories(app PRIVATE include)
endif()

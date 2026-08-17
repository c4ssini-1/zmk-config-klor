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

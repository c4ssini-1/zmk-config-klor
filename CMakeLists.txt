# Sources go into the "app" target rather than a zephyr_library().
#
# ZMK's app/CMakeLists.txt does "target_include_directories(app PRIVATE include)",
# so app/include -- which holds zmk/event_manager.h and zmk/events/* -- is not
# exported to modules. A standalone zephyr_library() cannot see those headers
# and fails with "zmk/event_manager.h: No such file or directory".
#
# Compiling into app inherits its private include paths. This is what the
# established community modules do (zmk-rgbled-widget, zmk-adaptive-key).
target_sources_ifdef(CONFIG_KLOR_RGB_REACTIVE app PRIVATE src/klor_rgb_reactive.c)

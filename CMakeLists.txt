# TEMPORARY -- remove with the harness. See Kconfig / src/klor_led_map.c.
if(CONFIG_KLOR_LED_MAP_HARNESS)
  zephyr_library()
  zephyr_library_sources(src/klor_led_map.c)
endif()

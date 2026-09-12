# Haven: only the load-switch bring-up is kept from upstream's board code
# (no MCUboot hook, no HFCLK divider tweak).
if(CONFIG_BOARD_OPENEARABLE_V2_NRF5340_CPUAPP AND NOT CONFIG_MCUBOOT)
    zephyr_library()
    zephyr_library_sources(board_init.c)
endif()

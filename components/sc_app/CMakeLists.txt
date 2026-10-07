# StreamCore32 application: streams, web UI, settings, device services and
# (optionally) the e-paper UI.  The parts follow menuconfig (StreamCore32).
set(srcs "src/app.cpp" "src/Settings.cpp" "src/CrashLog.cpp" "src/wifi_manager.cpp"
         "src/NvsCreds.cpp" "src/device.cpp")
# (requirements are resolved before Kconfig is read: always all of them;
#  the disabled ones are empty components)
set(reqs streamcore bell nvs_flash esp_wifi esp_netif esp_event esp_timer mdns
         espcoredump esp_driver_spi esp_driver_gpio esp_driver_rmt fatfs led_strip
         sc_spotify sc_qobuz sc_webstream sc_sdfile sc_dlna sc_sdcard
         i2c_bus ft6x36 bq27220 gdey027t91 eink_ui eink_vg Adafruit-GFX)
if(CONFIG_SC32_SDCARD)
  list(APPEND srcs "src/SdFiles.cpp")
endif()
if(CONFIG_SC32_STATUS_LED)
  list(APPEND srcs "src/SK6812.cpp")
endif()
if(CONFIG_SC32_DISPLAY_EINK)
  list(APPEND srcs "src/sc_ui_device.cpp")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" REQUIRES ${reqs})
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

# CrashLog: copy the panic handler's output into RAM that survives the reset
target_link_libraries(${COMPONENT_LIB} INTERFACE
  "-Wl,--wrap=panic_print_str" "-Wl,--wrap=panic_print_hex" "-Wl,--wrap=panic_print_dec")

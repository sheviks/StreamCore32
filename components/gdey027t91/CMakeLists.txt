# GDEY027T91 2.7" e-paper driver (SSD1680, 176x264) on its own SPI bus
set(srcs)
if(CONFIG_SC32_DISPLAY_EINK)
  set(srcs "epd.cpp" "epdspi.cpp" "gdey027T91.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
  REQUIRES Adafruit-GFX esp_driver_spi esp_driver_gpio)

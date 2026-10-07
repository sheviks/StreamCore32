# shared I2C master bus (touch controller, fuel gauge); compiled only when
# one of them is enabled
if(CONFIG_SC32_TOUCH OR CONFIG_SC32_BATTERY)
  set(srcs "i2c_bus.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include" REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_spi)

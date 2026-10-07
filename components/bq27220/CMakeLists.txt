# BQ27220 battery fuel gauge
set(srcs)
if(CONFIG_SC32_BATTERY)
  set(srcs "BQ27220.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
  REQUIRES i2c_bus bell streamcore esp_driver_i2c)

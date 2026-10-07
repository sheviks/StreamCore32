# FT6X36 capacitive touch controller.  The header (touch frame types) is
# always available: the e-paper UI uses them even without a touch panel.
if(CONFIG_SC32_TOUCH)
  set(srcs "FT6X36.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
  REQUIRES i2c_bus esp_driver_gpio esp_driver_i2c esp_timer)

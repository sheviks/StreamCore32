# SD card (SDMMC 1-bit, FAT at /sdcard, hot plug)
set(srcs)
if(CONFIG_SC32_SDCARD)
  set(srcs "SD_Master.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
  REQUIRES streamcore fatfs sdmmc esp_driver_sdmmc esp_driver_gpio)

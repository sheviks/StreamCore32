# DLNA / UPnP media renderer (enable: menuconfig StreamCore32 -> Streaming sources)
set(srcs)
if(CONFIG_SC32_DLNA)
  file(GLOB srcs "src/*.cpp")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       REQUIRES streamcore bell esp-tls mbedtls lwip esp_timer)

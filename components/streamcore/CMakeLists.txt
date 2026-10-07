# StreamCore32 core: StreamBase (control API of every source), AudioControl
# (feeds the VS1053), web UI, zeroconf/mDNS helpers, logging, time sync.
file(GLOB CORE_SRCS "src/*.cpp")
idf_component_register(
  SRCS ${CORE_SRCS}
  INCLUDE_DIRS "include"
  REQUIRES bell mdns nvs_flash esp_timer mbedtls efuse lwip pthread)

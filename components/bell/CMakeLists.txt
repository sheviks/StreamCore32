# bell — reduced copy of https://github.com/feelfreelinux/bell
# (only what StreamCore32 uses on the ESP32: HTTP(S) client, civetweb server,
#  sockets/TLS, utilities, nanopb, cJSON, nlohmann_json and the VS1053 sink).
# Options come from Kconfig (menuconfig: StreamCore32 -> Audio output (VS1053)).

file(GLOB BELL_SRCS
  "main/io/*.cpp" "main/io/*.c"
  "main/utilities/*.cpp" "main/utilities/*.c"
  "main/platform/esp/*.cpp"
  "main/audio-sinks/esp/VS1053.cpp"
  "external/nanopb/pb_common.c" "external/nanopb/pb_decode.c" "external/nanopb/pb_encode.c"
  "external/cJSON/cJSON.c"
  "external/civetweb/civetweb.c" "external/civetweb/CivetServer.cpp")

idf_component_register(
  SRCS ${BELL_SRCS}
  INCLUDE_DIRS
    "main/io/include"
    "main/utilities/include"
    "main/platform"
    "main/audio-sinks/include/VS1053"
    "external/nanopb"
    "external/cJSON"
    "external/civetweb/include"
    "external/nlohmann_json/include"
  REQUIRES mbedtls esp_driver_spi esp_driver_gpio pthread lwip nvs_flash esp_timer mdns
  PRIV_REQUIRES esp_wifi efuse)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
  PB_ENABLE_MALLOC PB_FIELD_32BIT BELL_DISABLE_FMT BELL_DISABLE_CODECS
  USE_DEFAULT_STDLIB=1 TARGET_OS_IPHONE=0)
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error=format -Wno-format -Wno-stringop-overflow -Wno-stringop-overread
  -Wno-unused-variable -Wno-unused-function)

# path of the nanopb generator for components that compile .proto files
set(BELL_NANOPB_ROOT "${CMAKE_CURRENT_LIST_DIR}/external/nanopb" CACHE INTERNAL "")

# Qobuz Connect (enable: menuconfig StreamCore32 -> Streaming sources)
set(srcs)
if(CONFIG_SC32_QOBUZ)
  file(GLOB srcs "src/*.cpp" "src/*.c")
endif()
idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS "include"
  REQUIRES streamcore bell mbedtls nvs_flash)
if(CONFIG_SC32_QOBUZ)
  include(${COMPONENT_DIR}/../bell/nanopb.cmake)
  sc32_nanopb_generate(${COMPONENT_LIB} "${COMPONENT_DIR}/protobuf" OPTIONS_FILE)
endif()

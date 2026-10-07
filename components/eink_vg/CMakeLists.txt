include($ENV{IDF_PATH}/tools/cmake/version.cmake)


#set (srcs, SRCS "eink_vg.c")
set(public_requires)

idf_component_register(#SRCS ${srcs}
    INCLUDE_DIRS "include"
REQUIRES ${public_requires}
)

  find_package(Python3 REQUIRED)

  set(SVG_DIR  "${CMAKE_CURRENT_LIST_DIR}/assets/svg")
  set(GEN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(GEN_H    "${GEN_DIR}/vg_assets.h")
  set(SVG_TOOL "${CMAKE_CURRENT_LIST_DIR}/tools/svg_folder_to_vg_header.py")

  file(GLOB SVG_INPUTS "${SVG_DIR}/*.svg")

# ---- box size: Kconfig (menuconfig ... E-paper display -> Icons), default 64x64
if(DEFINED CONFIG_EINK_VG_BOX_W)
  set(EINK_VG_BOX_W ${CONFIG_EINK_VG_BOX_W})
  set(EINK_VG_BOX_H ${CONFIG_EINK_VG_BOX_H})
else()
  set(EINK_VG_BOX_W 64)
  set(EINK_VG_BOX_H 64)
endif()
set(EINK_VG_BOX "${EINK_VG_BOX_W}x${EINK_VG_BOX_H}")

  add_custom_command(
    OUTPUT "${GEN_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${SVG_TOOL}" "${SVG_DIR}" "${GEN_H}" --box "${EINK_VG_BOX}"
    DEPENDS ${SVG_INPUTS} "${SVG_TOOL}"
    COMMENT "Generating VG assets header from SVG folder"
    VERBATIM
  )

add_custom_target(vg_assets_gen DEPENDS "${GEN_H}")

add_dependencies(${COMPONENT_LIB} vg_assets_gen)

target_include_directories(${COMPONENT_LIB} INTERFACE "${GEN_DIR}")
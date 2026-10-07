# sc32_nanopb_generate(<target> <proto dir> [OPTIONS_PATH | OPTIONS_FILE])
#   Compiles every <proto dir>/*.proto with protoc + the nanopb generator and
#   adds the generated protobuf/<name>.pb.c to <target>.  Generated headers are
#   included as "protobuf/<name>.pb.h".  protoc must be installed (PATH, or
#   the PROTOC environment variable).
function(sc32_nanopb_generate target proto_dir opt_mode)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(nanopb_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/external/nanopb")
  set(gen_py "${nanopb_root}/generator/nanopb_generator.py")
  set(nanopb_proto "${nanopb_root}/generator/proto")
  find_program(PROTOC NAMES protoc protoc.exe HINTS $ENV{PROTOC} "C:/proto3/bin")
  if(NOT PROTOC)
    message(FATAL_ERROR "protoc not found. Install it or set the PROTOC env var.")
  endif()
  set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}")
  get_filename_component(proto_root "${proto_dir}" DIRECTORY)
  file(GLOB protos "${proto_dir}/*.proto")
  set(srcs)
  foreach(proto ${protos})
    get_filename_component(base "${proto}" NAME_WE)
    set(desc "${gen_dir}/${base}.pb.desc")
    set(gen_c "${gen_dir}/protobuf/${base}.pb.c")
    set(gen_h "${gen_dir}/protobuf/${base}.pb.h")
    if(opt_mode STREQUAL "OPTIONS_FILE")
      set(opt_args --options-file "${proto_dir}/${base}.options")
    else()
      set(opt_args --options-path "${proto_dir}")
    endif()
    add_custom_command(
      OUTPUT "${desc}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
      COMMAND "${PROTOC}" -I "${proto_root}" -I "${proto_dir}" -I "${nanopb_proto}"
              --include_imports --descriptor_set_out "${desc}" "${proto}"
      DEPENDS "${proto}" ${protos}
      VERBATIM)
    add_custom_command(
      OUTPUT "${gen_c}" "${gen_h}"
      COMMAND "${Python3_EXECUTABLE}" "${gen_py}" --output-dir "${gen_dir}" ${opt_args} "${desc}"
      DEPENDS "${desc}" "${gen_py}"
      VERBATIM)
    list(APPEND srcs "${gen_c}")
  endforeach()
  target_sources(${target} PRIVATE ${srcs})
  target_include_directories(${target} PUBLIC "${gen_dir}")
endfunction()

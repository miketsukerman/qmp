# SPDX-License-Identifier: Apache-2.0
# qmp_add_service(NAME <name> PROTO <path/to/svc.proto> [OUT_DIR <dir>] [SOURCES_VAR <var>])
#
# Runs protoc with protoc-gen-qmp. Creates INTERFACE target qmp_gen_<name> exposing the
# generated include dir and sets <var> to the generated .c source. Tool lookup:
#   protoc:          QMP_PROTOC, then `protoc` on PATH, then `python3 -m grpc_tools.protoc`
#   protoc-gen-qmp:  QMP_PROTOC_GEN_QMP, then on PATH, then `python3 -m protoc_gen_qmp`
#                    (dev fallback using ${PROJECT_SOURCE_DIR}/codegen on PYTHONPATH)

find_package(Python3 COMPONENTS Interpreter QUIET)

set(QMP_PROTOC "" CACHE FILEPATH "protoc executable (empty = auto)")
set(QMP_PROTOC_GEN_QMP "" CACHE FILEPATH "protoc-gen-qmp executable (empty = auto)")

function(_qmp_resolve_codegen_tools)
  if(QMP_PROTOC)
    set(_protoc_cmd "${QMP_PROTOC}")
  else()
    find_program(_protoc_path protoc)
    if(_protoc_path)
      set(_protoc_cmd "${_protoc_path}")
    elseif(Python3_Interpreter_FOUND)
      set(_protoc_cmd "${Python3_EXECUTABLE}" -m grpc_tools.protoc)
    else()
      message(FATAL_ERROR "protoc not found: install protobuf-compiler or grpcio-tools")
    endif()
  endif()
  if(QMP_PROTOC_GEN_QMP)
    set(_plugin "${QMP_PROTOC_GEN_QMP}")
  else()
    find_program(_plugin_path protoc-gen-qmp)
    if(_plugin_path)
      set(_plugin "${_plugin_path}")
    else()
      if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "protoc-gen-qmp not found and no python3 for the dev fallback")
      endif()
      set(_plugin "${CMAKE_BINARY_DIR}/protoc-gen-qmp")
      file(WRITE "${_plugin}"
        "#!/bin/sh\nexport PYTHONPATH=\"${PROJECT_SOURCE_DIR}/codegen\${PYTHONPATH:+:\$PYTHONPATH}\"\nexec \"${Python3_EXECUTABLE}\" -m protoc_gen_qmp \"\$@\"\n")
      file(CHMOD "${_plugin}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
           GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endif()
  endif()
  set(QMP_PROTOC_CMD "${_protoc_cmd}" PARENT_SCOPE)
  set(QMP_PLUGIN_PATH "${_plugin}" PARENT_SCOPE)
endfunction()

function(qmp_add_service)
  cmake_parse_arguments(ARG "" "NAME;PROTO;OUT_DIR;SOURCES_VAR" "" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_PROTO)
    message(FATAL_ERROR "qmp_add_service: NAME and PROTO are required")
  endif()
  _qmp_resolve_codegen_tools()
  get_filename_component(_proto_abs "${ARG_PROTO}" ABSOLUTE)
  get_filename_component(_proto_stem "${ARG_PROTO}" NAME_WE)
  if(NOT ARG_OUT_DIR)
    set(ARG_OUT_DIR "${CMAKE_BINARY_DIR}/gen/${ARG_NAME}")
  endif()
  file(MAKE_DIRECTORY "${ARG_OUT_DIR}")
  set(_outputs
    "${ARG_OUT_DIR}/${_proto_stem}.qmp.h"
    "${ARG_OUT_DIR}/${_proto_stem}.qmp.c"
    "${ARG_OUT_DIR}/${_proto_stem}.reflect.json")
  file(GLOB_RECURSE _plugin_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/codegen/protoc_gen_qmp/*.py"
    "${PROJECT_SOURCE_DIR}/codegen/protoc_gen_qmp/templates/*.j2")
  # Protos are addressed relative to the idl/ root so the generated output path is flat.
  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND ${QMP_PROTOC_CMD}
      "--plugin=protoc-gen-qmp=${QMP_PLUGIN_PATH}"
      "--qmp_out=flat:${ARG_OUT_DIR}"
      "-I${PROJECT_SOURCE_DIR}/third_party/up-spec/up-core-api"
      "-I${PROJECT_SOURCE_DIR}/idl"
      "${_proto_abs}"
    DEPENDS "${_proto_abs}" "${PROJECT_SOURCE_DIR}/idl/qmp/options.proto" ${_plugin_sources}
    COMMENT "protoc-gen-qmp: ${ARG_PROTO}"
    VERBATIM)
  add_custom_target(qmp_gen_${ARG_NAME}_files DEPENDS ${_outputs})
  add_library(qmp_gen_${ARG_NAME} INTERFACE)
  add_dependencies(qmp_gen_${ARG_NAME} qmp_gen_${ARG_NAME}_files)
  target_include_directories(qmp_gen_${ARG_NAME} INTERFACE "${ARG_OUT_DIR}")
  target_link_libraries(qmp_gen_${ARG_NAME} INTERFACE qmp::qmp)
  if(ARG_SOURCES_VAR)
    set(${ARG_SOURCES_VAR} "${ARG_OUT_DIR}/${_proto_stem}.qmp.c" PARENT_SCOPE)
  endif()
endfunction()

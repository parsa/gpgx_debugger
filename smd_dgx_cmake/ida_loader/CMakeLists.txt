# ---- Sega Genesis / Mega Drive ROM loader for IDA 9.2+ (optional) ----
#
# Gated on the same IDA_SDK_DIR as the debugger plugin. Loaders live in
# <IDA>/loaders/, not plugins/. No Qt and no emulator here — this module only
# builds the database (segments, header, vector table) that the debugger
# plugin then attaches to.

if (NOT IDA_SDK_DIR OR NOT IDA_LIB)
  return()
endif()

add_library(smd_dgx_ldr SHARED
  smd_dgx_ldr.cpp
  smd_dgx_ldr.h
)
target_include_directories(smd_dgx_ldr PRIVATE "${IDA_SDK_DIR}/include")
target_compile_definitions(smd_dgx_ldr PRIVATE
  __NT__ __X64__ __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_ldr PRIVATE "${IDA_LIB}")
set_target_properties(smd_dgx_ldr PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_ldr"
  SUFFIX ".dll"
)

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_ldr POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:smd_dgx_ldr>" "${IDA_INSTALL_DIR}/loaders/"
    COMMENT "Copy loader to IDA loaders dir")
endif()

cmake_minimum_required (VERSION 3.20)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("smd_dgx" LANGUAGES C CXX)

set(GX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

include(cmake/gx_options.cmake)
include(cmake/gx_core.cmake)
include(cmake/gx_libchdr.cmake)

gx_add_core(gx_core "${GX_ROOT}")

if (GX_ENABLE_CHD)
  gx_add_libchdr(gx_libchdr "${GX_ROOT}")
  target_link_libraries(gx_core PUBLIC gx_libchdr)
endif()

find_package(SDL3 CONFIG REQUIRED)  

add_executable (smd_dgx_cmake)

target_sources(smd_dgx_cmake PRIVATE "frontends/sdl/main_sdl.cpp")

target_link_libraries(smd_dgx_cmake PRIVATE gx_core SDL3::SDL3)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET smd_dgx_cmake PROPERTY CXX_STANDARD 20)
endif()

source_group(TREE "${GX_ROOT}/core" PREFIX "src\\core"      FILES ${CORE_C})
source_group(TREE "${GX_ROOT}/core" PREFIX "includes\\core" FILES ${CORE_H})
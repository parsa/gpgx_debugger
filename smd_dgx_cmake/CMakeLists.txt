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

add_library(gx_platform INTERFACE)
target_include_directories(gx_platform INTERFACE "frontends/sdl" "frontends/core")

gx_add_core(gx_core "${GX_ROOT}")

target_link_libraries(gx_core PUBLIC gx_platform)

find_package(ZLIB REQUIRED)
target_link_libraries(gx_core PRIVATE ZLIB::ZLIB)

if (GX_ENABLE_CHD)
  gx_add_libchdr(gx_libchdr "${GX_ROOT}")
  target_link_libraries(gx_core PUBLIC gx_libchdr)
endif()

add_library(debug_lib STATIC)
target_sources(debug_lib PRIVATE "debugger/debugger.cpp")

target_compile_definitions(debug_lib PRIVATE $<$<BOOL:${DEBUG_68K}>:DEBUG_68K>)

find_package(OpenGL REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(GX_PLATFORM_ROOT "${CMAKE_CURRENT_LIST_DIR}/frontends/sdl")
add_library(gx_platform_lib "${GX_PLATFORM_ROOT}/sdl.c" "${GX_PLATFORM_ROOT}/config.c" "${GX_PLATFORM_ROOT}/error.c" "${GX_PLATFORM_ROOT}/fileio.c" "${GX_PLATFORM_ROOT}/unzip.c")
target_link_libraries(gx_platform_lib PRIVATE gx_core)

add_executable(smd_dgx_cmake)
target_sources(smd_dgx_cmake PRIVATE "frontends/sdl/osd.h" "frontends/sdl/main_sdl2.cpp")

target_link_libraries(smd_dgx_cmake PRIVATE gx_core gx_platform_lib debug_lib SDL2::SDL2 OpenGL::GL imgui::imgui)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_target_properties(smd_dgx_cmake PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
  set_target_properties(debug_lib PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
endif()
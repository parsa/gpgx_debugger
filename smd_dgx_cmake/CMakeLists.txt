cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("smd_dgx_cmake" LANGUAGES C CXX)

set(GX_ROOT    "${CMAKE_CURRENT_LIST_DIR}/..")

file(GLOB_RECURSE CORE_C CONFIGURE_DEPENDS "${GX_ROOT}/core/*.c")
file(GLOB_RECURSE CORE_H CONFIGURE_DEPENDS "${GX_ROOT}/core/*.h")
file(GLOB_RECURSE GX_C   CONFIGURE_DEPENDS "${GX_ROOT}/gx/*.c")
file(GLOB_RECURSE GX_H   CONFIGURE_DEPENDS "${GX_ROOT}/gx/*.h")

list(FILTER GX_C EXCLUDE REGEX ".*/osd_.*\\.c$")
list(FILTER GX_H EXCLUDE REGEX ".*/gx/osd\\.h$")

set(SDL_H "${GX_ROOT}/sdl/osd.h")

# --- EXCLUDE third-party examples/tests/docs ---

# zstd examples / build artifacts
list(FILTER CORE_C EXCLUDE REGEX ".*/zstd-[^/]*/build/.*")
list(FILTER CORE_C EXCLUDE REGEX ".*/zstd-[^/]*/examples?/.*")
list(FILTER CORE_C EXCLUDE REGEX ".*/zstd-[^/]*/doc/.*")

# zlib examples / tests
list(FILTER CORE_C EXCLUDE REGEX ".*/zlib-[^/]*/examples?/.*")
list(FILTER CORE_C EXCLUDE REGEX ".*/zlib-[^/]*/test/.*")

# generic safety net
list(FILTER CORE_C EXCLUDE REGEX ".*/(test|tests|examples?|doc)/.*")

find_package(SDL3 CONFIG REQUIRED)  

add_executable (smd_dgx_cmake)

target_sources(smd_dgx_cmake PRIVATE
  ${CORE_C} ${CORE_H}
  ${GX_C}   ${GX_H}
)

target_include_directories(smd_dgx_cmake PRIVATE
  "${GX_ROOT}/gx"
  "${GX_ROOT}/gx/fileio"
  "${GX_ROOT}/core"
  "${GX_ROOT}/core/cd_hw"
  "${GX_ROOT}/core/cart_hw"
  "${GX_ROOT}/core/cart_hw/svp"
  "${GX_ROOT}/core/input_hw"
  "${GX_ROOT}/core/debug"
  "${GX_ROOT}/core/ntsc"
  "${GX_ROOT}/core/m68k"
  "${GX_ROOT}/core/z80"
  "${GX_ROOT}/core/sound"

  # libchdr public headers
  "${GX_ROOT}/core/cd_hw/libchdr/include"

  # libchdr deps
  "${GX_ROOT}/core/cd_hw/libchdr/deps/lzma-24.05/include"
  "${GX_ROOT}/core/cd_hw/libchdr/deps/zlib-1.3.1"
  "${GX_ROOT}/core/cd_hw/libchdr/deps/zstd-1.5.6/lib"
  "${GX_ROOT}/core/cd_hw/libchdr/deps/zstd-1.5.6/lib/common"
)

target_link_libraries(smd_dgx_cmake PRIVATE SDL3::SDL3)

target_compile_definitions(smd_dgx_cmake PRIVATE
  LSB_FIRST
  USE_16BPP_RENDERING
  USE_LIBTREMOR
  USE_LIBCHDR
  HAVE_YM3438_CORE
  MAXROMSIZE=33554432
)

if(MSVC)
  target_compile_definitions(smd_dgx_cmake PRIVATE
    _CRT_SECURE_NO_WARNINGS
  )
endif()

# Third-party
target_compile_definitions(smd_dgx_cmake PRIVATE
  Z7_ST
  ZSTD_DISABLE_ASM
)

target_compile_definitions(smd_dgx_cmake PRIVATE
  INLINE=static\ inline
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET smd_dgx_cmake PROPERTY CXX_STANDARD 20)
endif()

source_group(TREE "${GX_ROOT}/core" PREFIX "src\\core"      FILES ${CORE_C})
source_group(TREE "${GX_ROOT}/core" PREFIX "includes\\core" FILES ${CORE_H})
source_group(TREE "${GX_ROOT}/gx"   PREFIX "src"            FILES ${GX_C})
source_group(TREE "${GX_ROOT}/gx"   PREFIX "includes"       FILES ${GX_H})

source_group(TREE "${GX_ROOT}/sdl"  PREFIX "includes\\sdl" FILES ${SDL_H})
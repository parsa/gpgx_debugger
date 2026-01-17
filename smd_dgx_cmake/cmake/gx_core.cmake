# GX Core as static library

function(gx_add_core target GX_ROOT)
  set(GENPLUS_SRC_DIR
    "${GX_ROOT}/core"
    "${GX_ROOT}/core/z80"
    "${GX_ROOT}/core/m68k"
    "${GX_ROOT}/core/ntsc"
    "${GX_ROOT}/core/sound"
    "${GX_ROOT}/core/input_hw"
    "${GX_ROOT}/core/cd_hw"
    "${GX_ROOT}/core/cart_hw"
    "${GX_ROOT}/core/cart_hw/svp"
  )

  if (GX_HOOK_CPU)
    list(APPEND GENPLUS_SRC_DIR "${GX_ROOT}/core/debug")
  endif()

  set(CORE_SOURCES "")
  foreach(dir IN LISTS GENPLUS_SRC_DIR)
    file(GLOB dir_sources CONFIGURE_DEPENDS "${dir}/*.c")
    list(APPEND CORE_SOURCES ${dir_sources})
  endforeach()

  add_library(${target} STATIC ${CORE_SOURCES})

  target_compile_definitions(${target} PUBLIC
    _CRT_SECURE_NO_WARNINGS
    LSB_FIRST
    USE_16BPP_RENDERING
    MAXROMSIZE=33554432
    HAVE_YM3438_CORE
    Z7_ST
    ZSTD_DISABLE_ASM
    INLINE=static inline
    $<$<BOOL:${GX_HOOK_CPU}>:HOOK_CPU>
    $<$<AND:$<BOOL:${GX_ENABLE_CHD}>,$<PLATFORM_ID:Windows>>:HAVE_FSEEKO>
  )

  target_include_directories(${target} PUBLIC
    ${GENPLUS_SRC_DIR}
  )
endfunction()
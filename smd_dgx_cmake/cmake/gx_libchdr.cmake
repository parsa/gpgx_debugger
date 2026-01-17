# Deps

function(gx_add_libchdr target GX_ROOT)
  set(CHDLIBDIR "${GX_ROOT}/core/cd_hw/libchdr")

  add_library(${target} STATIC)

  target_sources(${target} PRIVATE
    "${CHDLIBDIR}/src/bitstream.c"
    "${CHDLIBDIR}/src/cdrom.c"
    "${CHDLIBDIR}/src/chd.c"
    "${CHDLIBDIR}/src/flac.c"
    "${CHDLIBDIR}/src/huffman.c"

    # deps/libFLAC
    "${CHDLIBDIR}/deps/libFLAC/bitmath.c"
    "${CHDLIBDIR}/deps/libFLAC/bitreader.c"
    "${CHDLIBDIR}/deps/libFLAC/cpu.c"
    "${CHDLIBDIR}/deps/libFLAC/crc.c"
    "${CHDLIBDIR}/deps/libFLAC/fixed.c"
    "${CHDLIBDIR}/deps/libFLAC/float.c"
    "${CHDLIBDIR}/deps/libFLAC/format.c"
    "${CHDLIBDIR}/deps/libFLAC/lpc.c"
    "${CHDLIBDIR}/deps/libFLAC/lpc_intrin_avx2.c"
    "${CHDLIBDIR}/deps/libFLAC/lpc_intrin_sse.c"
    "${CHDLIBDIR}/deps/libFLAC/lpc_intrin_sse2.c"
    "${CHDLIBDIR}/deps/libFLAC/lpc_intrin_sse41.c"
    "${CHDLIBDIR}/deps/libFLAC/md5.c"
    "${CHDLIBDIR}/deps/libFLAC/memory.c"
    "${CHDLIBDIR}/deps/libFLAC/stream_decoder.c"

    # deps/lzma
    "${CHDLIBDIR}/deps/lzma/LzFind.c"
    "${CHDLIBDIR}/deps/lzma/LzmaDec.c"
    "${CHDLIBDIR}/deps/lzma/LzmaEnc.c"
  )

  target_include_directories(${target} PUBLIC
    "${CHDLIBDIR}/src"
    "${CHDLIBDIR}/deps/libFLAC/include"
    "${CHDLIBDIR}/deps/lzma"
    "${CHDLIBDIR}/deps/zlib"
  )
endfunction()
# ---- IDA 9.2+ debugger plugin (optional; requires the IDA SDK) ----
#
# Configure with:  -DIDA_SDK_DIR=<path to unpacked SDK>  (folder with include/, lib/)
# Optionally:      -DIDA_INSTALL_DIR=<IDA install>       (for a post-build copy to plugins/)
#
# IDA 9.x is x64-only and ea64-only. Toolchain: MSVC v143 (same as the rest of
# the project), so statically linking smd_dgx_emu is ABI-clean.
# VERIFY-9.3: lib subdir name below matches the 9.1 SDK layout (x64_win_vc_64);
# confirm against the 9.3 SDK when installed.

set(IDA_SDK_DIR "" CACHE PATH "Path to IDA SDK 9.2+ (contains include/ and lib/)")
set(IDA_INSTALL_DIR "" CACHE PATH "Optional: IDA install dir for post-build plugin copy")

if (NOT IDA_SDK_DIR)
  message(STATUS "IDA plugin: skipped (set IDA_SDK_DIR to enable)")
  return()
endif()
if (NOT EXISTS "${IDA_SDK_DIR}/include/ida.hpp")
  message(WARNING "IDA plugin: ${IDA_SDK_DIR}/include/ida.hpp not found — target skipped")
  return()
endif()

find_library(IDA_LIB
  NAMES ida ida64
  HINTS "${IDA_SDK_DIR}/lib/x64_win_vc_64"
        "${IDA_SDK_DIR}/lib/x64_win_vc_64_teams"
        "${IDA_SDK_DIR}/lib"
)
if (NOT IDA_LIB)
  message(WARNING "IDA plugin: ida.lib not found under ${IDA_SDK_DIR}/lib — target skipped")
  return()
endif()

add_library(smd_dgx_ida SHARED
  smd_dgx_ida.cpp
  ida_registers.h
)
target_include_directories(smd_dgx_ida PRIVATE
  "${IDA_SDK_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/.."       # debugger/EmuHost.h etc.
)
target_compile_definitions(smd_dgx_ida PRIVATE
  __NT__ __X64__ __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_ida PRIVATE smd_dgx_emu "${IDA_LIB}")
set_target_properties(smd_dgx_ida PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_ida"
  SUFFIX ".dll"
)

# Views embedding (phase 3): IDA 9.2+ ships stock Qt 6.8.2 — the same Qt our
# views target, so they are compiled straight into the plugin. At runtime the
# plugin uses the Qt DLLs already loaded by IDA (do NOT deploy Qt next to it).
if (Qt6_FOUND)
  set(GX_QT6_VIEWS "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6/views")
  target_sources(smd_dgx_ida PRIVATE
    ida_dock.cpp        # IDA side, no Qt
    ida_views.cpp       # Qt side, no IDA headers
    "${GX_QT6_VIEWS}/VdpRamView.cpp"
    "${GX_QT6_VIEWS}/VdpRegView.cpp"
    "${GX_QT6_VIEWS}/VdpSpritesView.cpp"
    "${GX_QT6_VIEWS}/PlaneExplorerView.cpp"
    "${GX_QT6_VIEWS}/SoundDebugView.cpp"
    "${GX_QT6_VIEWS}/MemoryView.cpp"
    "${GX_QT6_VIEWS}/RamSearchView.cpp"
    "${GX_QT6_VIEWS}/RamWatchView.cpp"
  )
  target_include_directories(smd_dgx_ida PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6")
  # NOTE: no QT_NO_KEYWORDS — views use signals/slots/emit. If the 9.3 SDK
  # headers clash with Qt keywords, switch views to Q_SIGNALS/Q_SLOTS/Q_EMIT.
  target_compile_definitions(smd_dgx_ida PRIVATE SMD_DGX_IDA_VIEWS)
  target_link_libraries(smd_dgx_ida PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core)
  set_target_properties(smd_dgx_ida PROPERTIES AUTOMOC ON)
endif()

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_ida POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:smd_dgx_ida>" "${IDA_INSTALL_DIR}/plugins/"
    COMMENT "Copy plugin to IDA plugins dir")
endif()

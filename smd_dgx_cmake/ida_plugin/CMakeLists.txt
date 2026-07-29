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
  HINTS "${IDA_SDK_DIR}/lib/x64_win_vc_64"        # licensed SDK layout
        "${IDA_SDK_DIR}/lib/x64_win_vc_64_teams"
        "${IDA_SDK_DIR}/lib/x64_win_64"           # open-source ida-sdk layout
        "${IDA_SDK_DIR}/lib"
)
if (NOT IDA_LIB)
  message(WARNING "IDA plugin: ida.lib not found under ${IDA_SDK_DIR}/lib — target skipped")
  return()
endif()

add_library(smd_dgx_ida SHARED
  smd_dgx_ida.cpp
  ida_input.c          # headless input stub (osd_input_update -> no-op)
  ida_registers.h
  $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_LIST_DIR}/../platform/AudioOutput.cpp>
)
target_include_directories(smd_dgx_ida PRIVATE
  "${IDA_SDK_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/.."       # debugger/EmuHost.h etc.
)
target_compile_definitions(smd_dgx_ida PRIVATE
  __NT__ __X64__ __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_ida PRIVATE smd_dgx_emu "${IDA_LIB}"
  $<$<BOOL:${WIN32}>:winmm>)   # AudioOutput (waveOut)
set_target_properties(smd_dgx_ida PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_ida"
  SUFFIX ".dll"
)

# Views embedding (phase 3).
#
# IDA ships Qt 6.8.2, but built inside a namespace: its exports are
# ??0QString@QT@@... , not ??0QString@@... . Linking the stock Qt import libs
# produces a DLL whose every Qt import is unresolvable at load time ("The
# specified procedure could not be found"). So:
#   - headers + moc come from the regular Qt install (same 6.8.2), and
#   - QT_NAMESPACE=QT makes them declare everything in namespace QT, and
#   - we link the import libs the SDK ships for IDA's own Qt (lib/x64_win_qt).
# At runtime the plugin uses the Qt DLLs IDA has already loaded — never deploy
# Qt next to the plugin.
find_library(IDA_QT_CORE    Qt6Core    HINTS "${IDA_SDK_DIR}/lib/x64_win_qt" NO_DEFAULT_PATH)
find_library(IDA_QT_GUI     Qt6Gui     HINTS "${IDA_SDK_DIR}/lib/x64_win_qt" NO_DEFAULT_PATH)
find_library(IDA_QT_WIDGETS Qt6Widgets HINTS "${IDA_SDK_DIR}/lib/x64_win_qt" NO_DEFAULT_PATH)

if (Qt6_FOUND AND IDA_QT_CORE AND IDA_QT_GUI AND IDA_QT_WIDGETS)
  set(GX_QT6_VIEWS "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6/views")
  target_sources(smd_dgx_ida PRIVATE
    ida_dock.cpp        # IDA side, no Qt
    ida_views.cpp       # Qt side, no IDA headers
    "${GX_QT6_VIEWS}/EmulatorScreen.cpp"
    "${GX_QT6_VIEWS}/VdpRamView.cpp"
    "${GX_QT6_VIEWS}/VdpRegView.cpp"
    "${GX_QT6_VIEWS}/VdpSpritesView.cpp"
    "${GX_QT6_VIEWS}/PlaneExplorerView.cpp"
    "${GX_QT6_VIEWS}/SoundDebugView.cpp"
    "${GX_QT6_VIEWS}/MemoryView.cpp"
    "${GX_QT6_VIEWS}/RamSearchView.cpp"
    "${GX_QT6_VIEWS}/RamWatchView.cpp"
  )
  target_include_directories(smd_dgx_ida PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6"
    # Qt headers as plain includes; the Qt6:: targets are deliberately NOT
    # linked, only their include dirs are reused.
    $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Gui,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
  )
  # NOTE: no QT_NO_KEYWORDS — views use signals/slots/emit. If the 9.3 SDK
  # headers clash with Qt keywords, switch views to Q_SIGNALS/Q_SLOTS/Q_EMIT.
  target_compile_definitions(smd_dgx_ida PRIVATE
    SMD_DGX_IDA_VIEWS
    QT_NAMESPACE=QT              # match IDA's namespaced Qt build
    QT_CORE_LIB QT_GUI_LIB QT_WIDGETS_LIB
  )
  # Normally supplied by the Qt6:: targets, which we no longer link.
  if (MSVC)
    target_compile_options(smd_dgx_ida PRIVATE /Zc:__cplusplus /permissive-)
  endif()
  target_link_libraries(smd_dgx_ida PRIVATE
    "${IDA_QT_WIDGETS}" "${IDA_QT_GUI}" "${IDA_QT_CORE}")
  set_target_properties(smd_dgx_ida PROPERTIES AUTOMOC ON)
else()
  message(STATUS "IDA plugin: views disabled (need Qt6 + ${IDA_SDK_DIR}/lib/x64_win_qt)")
endif()

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_ida POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:smd_dgx_ida>" "${IDA_INSTALL_DIR}/plugins/"
    COMMENT "Copy plugin to IDA plugins dir")
endif()

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

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ida_platform.cmake")

find_library(IDA_LIB
  NAMES ida ida64
  HINTS ${IDA_LIB_HINTS} "${IDA_SDK_DIR}/lib"
)
if (NOT IDA_LIB)
  message(WARNING "IDA plugin: no ida library found under ${IDA_SDK_DIR}/lib — target skipped")
  return()
endif()

add_library(smd_dgx_ida SHARED
  smd_dgx_ida.cpp
  smd_dgx_ida_z80.cpp  # second debugger_t: attaches to a running emulator
  ida_z80_loader.cpp   # "Dump Z80 driver" action
  ida_input.c          # headless input stub (osd_input_update -> no-op)
  ida_patch.cpp        # ROM patching, IDA side (no Qt)
  ida_asm.cpp          # constant decoding for the listing
  ida_registers.h
)
target_include_directories(smd_dgx_ida PRIVATE
  "${IDA_SDK_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/.."       # debugger/EmuHost.h etc.
)
target_compile_definitions(smd_dgx_ida PRIVATE
  ${IDA_PLATFORM_DEF} ${IDA_PROCESSOR_DEF} __EA64__
  NO_OBSOLETE_FUNCS
)
target_link_libraries(smd_dgx_ida PRIVATE smd_dgx_emu gx_audio "${IDA_LIB}")
set_target_properties(smd_dgx_ida PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
  OUTPUT_NAME "smd_dgx_ida"
  PREFIX ""                       # IDA scans for the bare name, not libsmd_...
  SUFFIX "${IDA_MODULE_SUFFIX}"
)

# Views embedding (phase 3).
#
# IDA ships Qt (6.8.2 through 9.4, 6.12 in 9.5), but built inside a namespace: its exports are
# ??0QString@QT@@... , not ??0QString@@... . Linking the stock Qt import libs
# produces a DLL whose every Qt import is unresolvable at load time ("The
# specified procedure could not be found"). So:
#   - headers + moc come from the regular Qt install of the same version, and
#   - QT_NAMESPACE=QT makes them declare everything in namespace QT, and
#   - we link the import libs the SDK ships for IDA's own Qt (lib/*_qt).
# Never deploy a second Qt next to the plugin. On macOS explicitly link IDA's
# own frameworks: GUI IDA preloads them, but headless idat/idalib does not.
# Resolving against only preloaded symbols made the plugin unloadable in idat.
# On Linux the loader still resolves symbols against the host's Qt.
if (WIN32)
  find_library(IDA_QT_CORE    Qt6Core    HINTS ${IDA_QT_LIB_DIR} NO_DEFAULT_PATH)
  find_library(IDA_QT_GUI     Qt6Gui     HINTS ${IDA_QT_LIB_DIR} NO_DEFAULT_PATH)
  find_library(IDA_QT_WIDGETS Qt6Widgets HINTS ${IDA_QT_LIB_DIR} NO_DEFAULT_PATH)
  set(IDA_QT_READY ${IDA_QT_CORE})
  if (NOT IDA_QT_GUI OR NOT IDA_QT_WIDGETS)
    set(IDA_QT_READY OFF)
  endif()
elseif (APPLE)
  get_filename_component(_ida_macos_dir "${IDA_LIB}" DIRECTORY)
  get_filename_component(_ida_qt_default "${_ida_macos_dir}/../Frameworks" ABSOLUTE)
  set(IDA_QT_FRAMEWORK_DIR "${_ida_qt_default}" CACHE PATH
    "IDA's bundled Qt framework directory (macOS)")
  foreach(_qt_component Core Gui Widgets)
    string(TOUPPER "${_qt_component}" _qt_upper)
    set(IDA_QT_${_qt_upper} "${IDA_QT_FRAMEWORK_DIR}/Qt${_qt_component}.framework")
  endforeach()
  set(IDA_QT_READY ON)
  foreach(_qt_framework "${IDA_QT_CORE}" "${IDA_QT_GUI}" "${IDA_QT_WIDGETS}")
    if (NOT EXISTS "${_qt_framework}")
      message(FATAL_ERROR "IDA's Qt framework not found: ${_qt_framework}; set IDA_QT_FRAMEWORK_DIR to IDA.app/Contents/Frameworks")
    endif()
  endforeach()
else()
  set(IDA_QT_CORE "")
  set(IDA_QT_GUI "")
  set(IDA_QT_WIDGETS "")
  set(IDA_QT_READY ON)
endif()

if (Qt6_FOUND AND IDA_QT_READY)
  set(GX_QT6_VIEWS "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6/views")
  target_sources(smd_dgx_ida PRIVATE
    ida_dock.cpp        # IDA side, no Qt
    ida_views.cpp       # Qt side, no IDA headers
    # Same settings file as the standalone app, so zoom levels and watch lists
    # follow the user between the two hosts.
    "${CMAKE_CURRENT_LIST_DIR}/../frontends/qt6/ViewSettings.cpp"
    "${GX_QT6_VIEWS}/EmulatorScreen.cpp"
    "${GX_QT6_VIEWS}/SaveStateView.cpp"
    "${GX_QT6_VIEWS}/ScrollView.cpp"
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
  if (APPLE AND IDA_PROCESSOR_DEF STREQUAL "__ARM__" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Qt 6.8's __yield builtin path needs its ARM declaration before entering
    # namespace QT. moc sources include Qt before the SDK, unlike ordinary
    # module sources: do not depend on a hidden global CMAKE_CXX_FLAGS include.
    target_compile_options(smd_dgx_ida PRIVATE -include arm_acle.h)
  endif()
  if (WIN32 OR APPLE)
    target_link_libraries(smd_dgx_ida PRIVATE
      "${IDA_QT_WIDGETS}" "${IDA_QT_GUI}" "${IDA_QT_CORE}")
    if (APPLE)
      set_property(TARGET smd_dgx_ida APPEND PROPERTY BUILD_RPATH
        "@loader_path/../../Frameworks" "${IDA_QT_FRAMEWORK_DIR}")
      set_property(TARGET smd_dgx_ida APPEND PROPERTY INSTALL_RPATH
        "@loader_path/../../Frameworks")
    endif()
  endif()
  set_target_properties(smd_dgx_ida PROPERTIES AUTOMOC ON)
else()
  message(STATUS "IDA plugin: views disabled (need Qt6, and on Windows ${IDA_QT_LIB_DIR})")
endif()

if (IDA_INSTALL_DIR)
  add_custom_command(TARGET smd_dgx_ida POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -DSRC=$<TARGET_FILE:smd_dgx_ida> -DDST=${IDA_INSTALL_DIR}/plugins
            -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/install_ida_module.cmake"
    COMMENT "Install plugin into IDA")
endif()

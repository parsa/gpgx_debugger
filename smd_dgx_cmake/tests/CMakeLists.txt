# ---- Tests ----
#
# Links smd_dgx_emu, which is the whole debug layer plus the emulator core, and
# nothing else: no Qt, no IDA, no SDL. That is the point — this is the layer
# every host shares, so it is the layer worth pinning down, and it builds on
# every platform the project claims to support.

add_executable(smd_dgx_tests
  test_main.cpp
  test_backend.cpp
  test_bridge.cpp
  test_timing.c
  test_timing.cpp
  test_subscriptions.cpp
  test_cpu_state.cpp
  test_exceptions.cpp
  test_stop_reasons.cpp
  test_access_log.cpp
  test_run_limits.cpp
  test_host_sync.cpp
  test_overrides.cpp
  test_input.c          # headless osd_input_update, as every other host has
  test_harness.h
  synth_rom.h
  fixture.h
)
target_link_libraries(smd_dgx_tests PRIVATE smd_dgx_emu)
target_include_directories(smd_dgx_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(smd_dgx_tests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED YES
)

# Packaging tests use real Mach-O tools, never Python directly or downloads.
if (APPLE)
  find_program(SMD_DGX_UV_EXECUTABLE uv)
  if (SMD_DGX_UV_EXECUTABLE)
    add_test(NAME macos_packaging
      COMMAND "${SMD_DGX_UV_EXECUTABLE}" run --no-project --no-python-downloads python
              "${CMAKE_CURRENT_LIST_DIR}/test_package_macos.py")
    set_tests_properties(macos_packaging PROPERTIES TIMEOUT 60)
  endif()
endif()

add_test(NAME smd_dgx_tests COMMAND smd_dgx_tests)
add_test(NAME ida_platform_mac
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/test_ida_platform.cmake")
# A hung run-control wait is the failure mode worth guarding: every wait in the
# fixture has its own deadline, this is the backstop if one is ever forgotten.
set_tests_properties(smd_dgx_tests PROPERTIES TIMEOUT 300)

# A clean arm64 build must compile moc-like Qt headers without a hidden
# CMAKE_CXX_FLAGS workaround. Reuse the IDA target's actual options/defines.
if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND TARGET smd_dgx_ida)
  get_target_property(_ida_defs smd_dgx_ida COMPILE_DEFINITIONS)
  if ("__ARM__" IN_LIST _ida_defs AND "SMD_DGX_IDA_VIEWS" IN_LIST _ida_defs)
    add_library(ida_arm_qt_compile_probe OBJECT ida_arm_qt_compile_probe.cpp)
    target_include_directories(ida_arm_qt_compile_probe PRIVATE
      $<TARGET_PROPERTY:smd_dgx_ida,INCLUDE_DIRECTORIES>)
    target_compile_definitions(ida_arm_qt_compile_probe PRIVATE
      $<TARGET_PROPERTY:smd_dgx_ida,COMPILE_DEFINITIONS>)
    target_compile_options(ida_arm_qt_compile_probe PRIVATE
      $<TARGET_PROPERTY:smd_dgx_ida,COMPILE_OPTIONS>)
    set_target_properties(ida_arm_qt_compile_probe PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    add_test(NAME ida_arm_qt_compile
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target ida_arm_qt_compile_probe)
    set_tests_properties(ida_arm_qt_compile PROPERTIES TIMEOUT 60)
  endif()
endif()

cmake_minimum_required(VERSION 3.14)
project(simple_query_windows LANGUAGES CXX)

set(PLUGIN_NAME "simple_query_windows_plugin")

# Built into both the plugin library and the native test runner.
set(PLUGIN_SOURCES
  "simple_query_windows_plugin.cpp"
  "background_calls.cpp"
  "boundary_formats.cpp"
  "detached_threads.cpp"
  "domain_snapshots.cpp"
  "file_mutations.cpp"
  "file_records.cpp"
  "host_call_work.cpp"
  "native_query.g.cpp"
  "observe_event_sender.cpp"
  "platform_thread_dispatcher.cpp"
  "polling_observers.cpp"
  "query_shaping.cpp"
  "request_values.cpp"
  "rows_result.cpp"
  "structured_log.cpp"
  "winrt_records.cpp"
)

add_library(${PLUGIN_NAME} SHARED
  "simple_query_windows_plugin_c_api.cpp"
  ${PLUGIN_SOURCES}
)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
# NOMINMAX keeps windows.h from defining min and max macros, which break
# std::min and std::max calls in the plugin source.
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL NOMINMAX)
# C++/WinRT needs C++20 for standard coroutines; under C++17 it falls back to
# <experimental/coroutine>, which current MSVC rejects (STL1011). PRIVATE keeps
# the consuming app's runner on its own language standard.
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_20)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin windowsapp)

set(simple_query_windows_bundled_libraries
  ""
  PARENT_SCOPE
)

# === Tests ===
# Only a host app that sets include_simple_query_windows_tests builds the test
# runner, so apps that use the plugin never download or build GoogleTest.
# tool/build_windows_consumer.sh sets it and runs the runner.
if (${include_${PROJECT_NAME}_tests})
  set(TEST_RUNNER "${PROJECT_NAME}_test")

  include(FetchContent)
  # Pinned by archive hash, so a changed download fails the build.
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    URL_HASH SHA256=40d4ec942217dcc84a9ebe2a68584ada7d4a33a8ee958755763278ea1c5e18ff
  )
  # Keep the host app's C runtime and leave GoogleTest out of the app bundle.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
  FetchContent_MakeAvailable(googletest)

  # A DLL holding its own copy of the dispatcher, so a test can check that the
  # dispatcher registers its window class for the module that holds it, as
  # the plugin DLL does in an app.
  set(DISPATCHER_PROBE "${PROJECT_NAME}_dispatcher_probe")
  add_library(${DISPATCHER_PROBE} SHARED
    "test/dispatcher_probe.cpp"
    "platform_thread_dispatcher.cpp"
  )
  apply_standard_settings(${DISPATCHER_PROBE})
  target_compile_definitions(${DISPATCHER_PROBE} PRIVATE NOMINMAX)
  target_compile_features(${DISPATCHER_PROBE} PRIVATE cxx_std_20)
  target_include_directories(${DISPATCHER_PROBE} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

  # The C API wrapper adds nothing to test, so the runner builds the plugin
  # sources directly instead of loading the plugin DLL.
  add_executable(${TEST_RUNNER}
    "test/background_calls_test.cpp"
    "test/file_mutations_test.cpp"
    "test/host_call_work_test.cpp"
    "test/platform_thread_dispatcher_test.cpp"
    "test/polling_observers_test.cpp"
    "test/query_shaping_test.cpp"
    "test/records_test.cpp"
    "test/simple_query_windows_plugin_test.cpp"
    "test/structured_log_test.cpp"
    "test/winrt_records_test.cpp"
    ${PLUGIN_SOURCES}
  )
  add_dependencies(${TEST_RUNNER} ${DISPATCHER_PROBE})
  apply_standard_settings(${TEST_RUNNER})
  target_compile_definitions(${TEST_RUNNER} PRIVATE NOMINMAX)
  target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_20)
  target_include_directories(${TEST_RUNNER} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  # ole32 provides CoGetApartmentType, which the apartment tests call.
  target_link_libraries(${TEST_RUNNER} PRIVATE
    flutter_wrapper_plugin windowsapp ole32 gtest_main)
  # flutter_wrapper_plugin links against the Flutter DLL, and the dispatcher
  # test loads the probe DLL from the runner's directory.
  add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FLUTTER_LIBRARY}" $<TARGET_FILE:${DISPATCHER_PROBE}>
    $<TARGET_FILE_DIR:${TEST_RUNNER}>
  )
endif()

cmake_minimum_required(VERSION 3.14)
project(simple_query_windows LANGUAGES CXX)

set(PLUGIN_NAME "simple_query_windows_plugin")

add_library(${PLUGIN_NAME} SHARED
  "simple_query_windows_plugin.cpp"
  "simple_query_windows_plugin_c_api.cpp"
  "native_query.g.cpp"
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

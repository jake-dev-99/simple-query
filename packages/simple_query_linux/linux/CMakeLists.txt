cmake_minimum_required(VERSION 3.13)

project(simple_query_linux LANGUAGES CXX)

set(PLUGIN_NAME "simple_query_linux_plugin")

find_package(PkgConfig)

# EDS client libraries are optional - contacts/calendar degrade gracefully.
if(PkgConfig_FOUND)
  pkg_check_modules(EBOOK libebook-1.2)
  pkg_check_modules(ECAL libecal-2.0)
  pkg_check_modules(EDS libedataserver-1.2)
endif()

add_library(${PLUGIN_NAME} SHARED
  "simple_query_linux_plugin.cc"
  "native_query.g.cpp"
)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_compile_features(${PLUGIN_NAME} PUBLIC cxx_std_17)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)

if(EBOOK_FOUND AND EDS_FOUND)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE HAS_LIBEBOOK=1)
  target_include_directories(${PLUGIN_NAME} PRIVATE ${EBOOK_INCLUDE_DIRS} ${EDS_INCLUDE_DIRS})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${EBOOK_LIBRARIES} ${EDS_LIBRARIES})
endif()

if(ECAL_FOUND AND EDS_FOUND)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE HAS_LIBECAL=1)
  target_include_directories(${PLUGIN_NAME} PRIVATE ${ECAL_INCLUDE_DIRS} ${EDS_INCLUDE_DIRS})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${ECAL_LIBRARIES} ${EDS_LIBRARIES})
endif()

set(simple_query_linux_bundled_libraries
  ""
  PARENT_SCOPE
)

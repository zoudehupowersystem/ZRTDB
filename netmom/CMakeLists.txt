cmake_minimum_required(VERSION 3.16)

add_library(netmom STATIC
  NetmomDb.cpp
  BpaParser.cpp
  Topology.cpp
)

target_include_directories(netmom PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(netmom PUBLIC zrtdb)
target_compile_options(netmom PRIVATE -Wall -Wextra)

add_executable(zrtdb_bpa_import bpa_import.cpp)
target_link_libraries(zrtdb_bpa_import PRIVATE netmom)
target_compile_options(zrtdb_bpa_import PRIVATE -Wall -Wextra)

add_executable(zrtdb_topology topology_main.cpp)
target_link_libraries(zrtdb_topology PRIVATE netmom)
target_compile_options(zrtdb_topology PRIVATE -Wall -Wextra)

install(TARGETS netmom
        ARCHIVE DESTINATION "${ZRTDB_INSTALL_ROOT}")
install(TARGETS zrtdb_bpa_import zrtdb_topology
        RUNTIME DESTINATION "${ZRTDB_INSTALL_BINDIR}")

if(BUILD_TESTING)
  add_executable(netmom_tests tests/netmom_tests.cpp)
  target_link_libraries(netmom_tests PRIVATE netmom)
  target_compile_options(netmom_tests PRIVATE -Wall -Wextra)
  add_test(NAME netmom_tests COMMAND netmom_tests)
endif()

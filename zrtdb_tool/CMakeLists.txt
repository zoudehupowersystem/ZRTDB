cmake_minimum_required(VERSION 3.16)
project(zrtdb_tool LANGUAGES CXX)

add_executable(zrtdb_tool
  main.cpp
  DbIoShell.cpp
  TableViewRenderer.cpp
  QueryEngine.cpp
)

target_include_directories(zrtdb_tool PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../zrtdb_lib
)

target_link_libraries(zrtdb_tool PRIVATE zrtdb)
target_compile_features(zrtdb_tool PRIVATE cxx_std_20)
target_compile_options(zrtdb_tool PRIVATE -Wall -Wextra)

install(TARGETS zrtdb_tool
        RUNTIME DESTINATION "${ZRTDB_INSTALL_BINDIR}")


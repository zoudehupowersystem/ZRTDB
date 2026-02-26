cmake_minimum_required(VERSION 3.16)
project(zrtdb_model LANGUAGES CXX)

add_executable(zrtdb_model
  main.cpp
  DatScanner.cpp
  DbCompiler.cpp
  DefIO.cpp
  Instantiator.cpp
)

target_include_directories(zrtdb_model PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../zrtdb_lib
)

target_link_libraries(zrtdb_model PRIVATE zrtdb)
target_compile_options(zrtdb_model PRIVATE -Wall -Wextra)

install(TARGETS zrtdb_model
        RUNTIME DESTINATION "${ZRTDB_INSTALL_BINDIR}")

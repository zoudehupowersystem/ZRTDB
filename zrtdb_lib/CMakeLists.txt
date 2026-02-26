cmake_minimum_required(VERSION 3.16)
project(zrtdb_rtdblib LANGUAGES CXX)

add_library(zrtdb STATIC
  StringUtils.cpp
  MetaIO.cpp
  MmdbManager.cpp
  LegacyShim.cpp
  SnapshotLock.cpp
  Snapshot.cpp
  SnapshotShim.cpp
  WatchdogClient.cpp
)

target_include_directories(zrtdb PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(zrtdb PRIVATE -Wall -Wextra)

# 关键：make install 时把 libzrtdb.a 放到 /usr/local/ZRTDB/
set_target_properties(zrtdb PROPERTIES OUTPUT_NAME "zrtdb")
install(TARGETS zrtdb
        ARCHIVE DESTINATION "${ZRTDB_INSTALL_ROOT}")


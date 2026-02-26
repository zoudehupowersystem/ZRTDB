cmake_minimum_required(VERSION 3.16)
project(ZRTDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------
# install layout (make install)
# ---------------------------
include(GNUInstallDirs)

# 安装根：默认 /usr/local/ZRTDB
set(ZRTDB_INSTALL_ROOT "${CMAKE_INSTALL_PREFIX}/ZRTDB"
    CACHE PATH "ZRTDB install root (default: /usr/local/ZRTDB)")

# 可执行文件安装目录：按你要求默认 /bin（可改为 /usr/local/bin）
set(ZRTDB_INSTALL_BINDIR "/bin"
    CACHE PATH "Install directory for executables (default: /bin)")

message(STATUS "ZRTDB_INSTALL_ROOT   = ${ZRTDB_INSTALL_ROOT}")
message(STATUS "ZRTDB_INSTALL_BINDIR = ${ZRTDB_INSTALL_BINDIR}")

# 1) 安装公共头文件到 /usr/local/ZRTDB/include
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/"
        DESTINATION "${ZRTDB_INSTALL_ROOT}/include"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# 2) 安装 DAT 到 /usr/local/ZRTDB/DAT
install(DIRECTORY "${CMAKE_SOURCE_DIR}/DAT/"
        DESTINATION "${ZRTDB_INSTALL_ROOT}/DAT"
        FILES_MATCHING PATTERN "*.DAT" PATTERN "APPDAT.json")

# 3) 预创建 header 目录（zrtdb_model 会在运行时写入）
install(CODE "file(MAKE_DIRECTORY \"${ZRTDB_INSTALL_ROOT}/header\")")
install(CODE "file(MAKE_DIRECTORY \"${ZRTDB_INSTALL_ROOT}/header/inc\")")
install(CODE "file(MAKE_DIRECTORY \"${ZRTDB_INSTALL_ROOT}/header/.datdef\")")

add_subdirectory(zrtdb_lib)
add_subdirectory(zrtdb_model)
add_subdirectory(zrtdb_tool)
add_subdirectory(zrtdb_watchdog)


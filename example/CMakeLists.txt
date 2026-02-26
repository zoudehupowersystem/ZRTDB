cmake_minimum_required(VERSION 3.16)
project(zrtdb_control_ipc_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ZRTDB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)


message(STATUS "ZRTDB_ROOT: ${ZRTDB_ROOT}")
add_executable(policy_gen_cpp policy_gen_cpp.cpp)
add_executable(policy_exec_cpp policy_exec_cpp.cpp)
add_executable(policy_gen policy_gen.cpp)
add_executable(policy_exec policy_exec.cpp)


foreach(tgt IN ITEMS policy_gen policy_exec policy_exec_cpp policy_gen_cpp)
  target_include_directories(${tgt} PRIVATE
    "${ZRTDB_ROOT}/include"
    "${ZRTDB_ROOT}/zrtdb_lib"
    "/usr/local/ZRTDB/header"
  )
  target_link_libraries(${tgt} PRIVATE /usr/local/ZRTDB/libzrtdb.a)
 
  if(UNIX)
    target_link_libraries(${tgt} PRIVATE pthread)
  endif()
endforeach()
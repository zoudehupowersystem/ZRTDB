add_executable(zrtdb_watchdog
    main.cpp
)

target_link_libraries(zrtdb_watchdog PRIVATE zrtdb)

target_include_directories(zrtdb_watchdog PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/zrtdb_lib
)

# Install to configured bindir (default: /bin)
install(TARGETS zrtdb_watchdog
        RUNTIME DESTINATION ${ZRTDB_INSTALL_BINDIR})

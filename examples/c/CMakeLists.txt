##############################################################################
# Bluetooth Example Executable
add_executable(c-bluetooth-example bluetoothExample.c)
target_include_directories(c-bluetooth-example PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/)
target_link_libraries(c-bluetooth-example interface-apis)

include(GNUInstallDirs)
install(TARGETS c-bluetooth-example
        RUNTIME
        DESTINATION ${CMAKE_INSTALL_BINDIR})

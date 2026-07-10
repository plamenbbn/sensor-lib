##############################################################################
# Demo App Executable
add_subdirectory(demo-app)

add_executable(cc-demo-app demoApp.cc)
target_include_directories(cc-demo-app PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/ demo-app/)
target_link_libraries(cc-demo-app demo-app interface-apis)

##############################################################################
# Bluetooth Example Executable
add_executable(cc-bluetooth-example bluetoothExample.cc)
target_include_directories(cc-bluetooth-example PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/)
target_link_libraries(cc-bluetooth-example interface-apis)

##############################################################################
# Link Example Executable
add_executable(cc-links-example linkExample.cc)
target_include_directories(cc-links-example PRIVATE ${CMAKE_SOURCE_DIR}/../interfaces/c/)
target_link_libraries(cc-links-example interface-apis)

##############################################################################
# Simulation Example Executable
add_executable(cc-simulation-example simulationExample.cc)
target_include_directories(cc-simulation-example PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/)
target_link_libraries(cc-simulation-example interface-apis)

##############################################################################
# Socket Example Executable
add_executable(cc-socket-example socketExample.cc)
target_include_directories(cc-socket-example PRIVATE
        ${CMAKE_SOURCE_DIR}/interfaces/c/
        ${CMAKE_SOURCE_DIR}/bratislava-socket/include/)
target_link_libraries(cc-socket-example interface-apis)

##############################################################################
# Telemetry Example Executable
add_executable(cc-telemetry-example telemetryExample.cc)
target_include_directories(cc-telemetry-example PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/)
target_link_libraries(cc-telemetry-example interface-apis yaml-cpp uuid)

##############################################################################
# Wi-Fi Example Executable
add_executable(cc-wifi-example wifiExample.cc)
target_include_directories(cc-wifi-example PRIVATE ${CMAKE_SOURCE_DIR}/interfaces/c/)
target_link_libraries(cc-wifi-example interface-apis)

##############################################################################
# Standalone Telemetry Example Executable (uses telemetry-api only)
add_executable(cc-standalone-telemetry-example standaloneTelemetryExample.cc)
target_link_libraries(cc-standalone-telemetry-example telemetry-api)

##############################################################################
# Install Executables
include(GNUInstallDirs)
install(TARGETS
        cc-bluetooth-example
        cc-links-example
        cc-simulation-example
        cc-socket-example
        cc-telemetry-example
        cc-wifi-example
        cc-standalone-telemetry-example
        cc-demo-app
        RUNTIME
        DESTINATION ${CMAKE_INSTALL_BINDIR})


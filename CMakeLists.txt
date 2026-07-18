cmake_minimum_required(VERSION 3.18)
project(sensor_lib LANGUAGES C CXX)

set(SENSOR_LIB_BLUETOOTH_SCANNER "bluez" CACHE STRING "Bluetooth discovery backend: bluez or hci")
set_property(CACHE SENSOR_LIB_BLUETOOTH_SCANNER PROPERTY STRINGS bluez hci)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBNL QUIET libnl-3.0 libnl-genl-3.0)
endif()

if(LIBNL_FOUND)
    set(SENSOR_LIB_WIFI_INCLUDE_DIRS ${LIBNL_INCLUDE_DIRS})
    set(SENSOR_LIB_WIFI_LIBRARY_DIRS ${LIBNL_LIBRARY_DIRS})
    set(SENSOR_LIB_WIFI_LIBRARIES ${LIBNL_LIBRARIES})
else()
    find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATHS /usr/include/libnl3)
    find_library(LIBNL3_LIBRARY NAMES nl-3 libnl-3)
    find_library(LIBNL_GENL3_LIBRARY NAMES nl-genl-3 libnl-genl-3)

    if(LIBNL_INCLUDE_DIR AND LIBNL3_LIBRARY AND LIBNL_GENL3_LIBRARY)
        set(SENSOR_LIB_WIFI_INCLUDE_DIRS ${LIBNL_INCLUDE_DIR})
        set(SENSOR_LIB_WIFI_LIBRARIES ${LIBNL3_LIBRARY} ${LIBNL_GENL3_LIBRARY})
    else()
        message(FATAL_ERROR "libnl-3 and libnl-genl-3 development files are required for Wi-Fi scanning")
    endif()
endif()

add_library(sensor-lib STATIC
    src/comms_runtime.cpp
    src/instrument_api.c
    src/wifi_scanner_bridge.cpp
    bratislava-socket/src/BratislavaSocket.cc
    bratislava-socket/src/BratislavaSocketRegistry.cc
    bratislava-socket/src/adapters/bluetooth/BluetoothAdapter.cc
    bratislava-socket/src/adapters/wifi/WifiAdapter.cc
)

add_library(sensor-lib-shared SHARED
    src/comms_runtime.cpp
    src/instrument_api.c
    src/wifi_scanner_bridge.cpp
    bratislava-socket/src/BratislavaSocket.cc
    bratislava-socket/src/BratislavaSocketRegistry.cc
    bratislava-socket/src/adapters/bluetooth/BluetoothAdapter.cc
    bratislava-socket/src/adapters/wifi/WifiAdapter.cc
)

if(SENSOR_LIB_BLUETOOTH_SCANNER STREQUAL "bluez")
    set(SENSOR_LIB_BLUETOOTH_SCANNER_SOURCE src/bluez_scanner.c)
    target_compile_definitions(sensor-lib PUBLIC SENSOR_LIB_BLUETOOTH_SCANNER_BLUEZ=1)
    target_compile_definitions(sensor-lib-shared PUBLIC SENSOR_LIB_BLUETOOTH_SCANNER_BLUEZ=1)
elseif(SENSOR_LIB_BLUETOOTH_SCANNER STREQUAL "hci")
    set(SENSOR_LIB_BLUETOOTH_SCANNER_SOURCE src/hci_scanner.c)
    target_compile_definitions(sensor-lib PUBLIC SENSOR_LIB_BLUETOOTH_SCANNER_HCI=1)
    target_compile_definitions(sensor-lib-shared PUBLIC SENSOR_LIB_BLUETOOTH_SCANNER_HCI=1)
else()
    message(FATAL_ERROR "SENSOR_LIB_BLUETOOTH_SCANNER must be either 'bluez' or 'hci'")
endif()

target_sources(sensor-lib PRIVATE ${SENSOR_LIB_BLUETOOTH_SCANNER_SOURCE})
target_sources(sensor-lib-shared PRIVATE ${SENSOR_LIB_BLUETOOTH_SCANNER_SOURCE})

target_include_directories(sensor-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/bratislava-socket/include
        ${CMAKE_CURRENT_SOURCE_DIR}/interfaces/c
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bratislava-socket/src
        ${SENSOR_LIB_WIFI_INCLUDE_DIRS}
)

target_include_directories(sensor-lib-shared
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/bratislava-socket/include
        ${CMAKE_CURRENT_SOURCE_DIR}/interfaces/c
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bratislava-socket/src
        ${SENSOR_LIB_WIFI_INCLUDE_DIRS}
)

target_compile_features(sensor-lib PUBLIC c_std_11)
target_compile_features(sensor-lib-shared PUBLIC c_std_11)
target_compile_features(sensor-lib PUBLIC cxx_std_17)
target_compile_features(sensor-lib-shared PUBLIC cxx_std_17)
target_compile_definitions(sensor-lib PRIVATE DEVICE_BUILD=1)
target_compile_definitions(sensor-lib-shared PRIVATE DEVICE_BUILD=1)
target_compile_options(sensor-lib PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sensor-lib-shared PRIVATE -Wall -Wextra -Wpedantic)
target_link_directories(sensor-lib PRIVATE ${SENSOR_LIB_WIFI_LIBRARY_DIRS})
target_link_directories(sensor-lib-shared PRIVATE ${SENSOR_LIB_WIFI_LIBRARY_DIRS})
find_package(Threads REQUIRED)
target_link_libraries(sensor-lib PUBLIC bluetooth ${SENSOR_LIB_WIFI_LIBRARIES} Threads::Threads)
target_link_libraries(sensor-lib-shared PUBLIC bluetooth ${SENSOR_LIB_WIFI_LIBRARIES} Threads::Threads)
set_target_properties(sensor-lib-shared PROPERTIES OUTPUT_NAME sensor-lib)

add_executable(instrument-cli
    tests/link_callback_harness.c
    tests/instrument_cli.c
)

target_link_libraries(instrument-cli PRIVATE sensor-lib)

add_executable(repeat-sensing-test
    tests/repeat_sensing_test.c
)

target_link_libraries(repeat-sensing-test PRIVATE sensor-lib)

add_executable(link-callback-test
    tests/link_callback_harness.c
    tests/link_callback_test.c
)

target_link_libraries(link-callback-test PRIVATE sensor-lib)

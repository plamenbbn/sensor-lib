cmake_minimum_required(VERSION 3.18)
project(sensor_lib LANGUAGES C)

add_library(sensor-lib STATIC
    src/instrument_api.c
)

add_library(sensor-lib-shared SHARED
    src/instrument_api.c
)

target_include_directories(sensor-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/interfaces/c
)

target_include_directories(sensor-lib-shared
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/interfaces/c
)

target_compile_features(sensor-lib PUBLIC c_std_11)
target_compile_features(sensor-lib-shared PUBLIC c_std_11)
target_compile_options(sensor-lib PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(sensor-lib-shared PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sensor-lib PUBLIC bluetooth)
target_link_libraries(sensor-lib-shared PUBLIC bluetooth)
set_target_properties(sensor-lib-shared PROPERTIES OUTPUT_NAME sensor-lib)

add_executable(instrument-cli
    tests/instrument_cli.c
)

target_link_libraries(instrument-cli PRIVATE sensor-lib)

add_executable(repeat-sensing-test
    tests/repeat_sensing_test.c
)

target_link_libraries(repeat-sensing-test PRIVATE sensor-lib)

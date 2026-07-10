cmake_minimum_required(VERSION 3.18)
project(sensor_lib LANGUAGES C)

add_library(sensor-lib STATIC
    src/instrument_api.c
)

target_include_directories(sensor-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/interfaces/c
)

target_compile_features(sensor-lib PUBLIC c_std_11)
target_compile_options(sensor-lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sensor-lib PUBLIC bluetooth)

add_executable(instrument-cli
    tests/instrument_cli.c
)

target_link_libraries(instrument-cli PRIVATE sensor-lib)

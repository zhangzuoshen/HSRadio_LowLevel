cmake_minimum_required(VERSION 3.16)
project(HSRadioHardware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core)
find_package(ALSA REQUIRED)

add_library(hsradio_hardware STATIC
    include/hsradio/HSRadioTypes.h
    include/hsradio/hardware/IRadioHardware.h
    include/hsradio/hardware/HSRadioSAF775XBackend.h
    src/hardware/HSRadioSAF775XBackend.cpp
)

target_include_directories(hsradio_hardware
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(hsradio_hardware
    PUBLIC Qt5::Core
    PRIVATE ALSA::ALSA
)

target_compile_options(hsradio_hardware PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

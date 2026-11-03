cmake_minimum_required(VERSION 3.16)
project(HSRadioHardware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 含 Q_OBJECT 的头文件需要 moc 自动处理
set(CMAKE_AUTOMOC ON)

# 是否编译 SAF775X 的 ALSA 真实后端（需要 ALSA 头 / 目标 sysroot）。
# 默认开启：交叉编译到目标平台时无需改动。
# 在无 ALSA 头的开发机（如 Ubuntu）上编译调试时传 -DHSRADIO_ENABLE_ALSA=OFF，
# 此时只编译 Mock 与业务层。
option(HSRADIO_ENABLE_ALSA "编译 SAF775X ALSA 真实后端（交叉编译/目标平台）" ON)

# 是否构建调度器冒烟测试（宿主环境运行，需配合 -DHSRADIO_ENABLE_ALSA=OFF）。
option(HSRADIO_BUILD_TESTS "构建调度器冒烟测试" OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core)

set(HSRADIO_SOURCES
    include/hsradio/HSRadioTypes.h
    include/hsradio/HSRadioSeekPolicy.h
    include/hsradio/hardware/IRadioHardware.h
    include/hsradio/hardware/MockRadioHardware.h
    include/hsradio/manager/HSRadioManager.h
    include/hsradio/scheduler/HSRadioOperationScheduler.h
    src/hardware/MockRadioHardware.cpp
    src/manager/HSRadioManager.cpp
    src/scheduler/HSRadioOperationScheduler.cpp
)

if(HSRADIO_ENABLE_ALSA)
    find_package(ALSA REQUIRED)
    list(APPEND HSRADIO_SOURCES
        include/hsradio/hardware/HSRadioSAF775XBackend.h
        src/hardware/HSRadioSAF775XBackend.cpp
    )
endif()

add_library(hsradio_hardware STATIC ${HSRADIO_SOURCES})

target_include_directories(hsradio_hardware
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(hsradio_hardware
    PUBLIC Qt5::Core
)

if(HSRADIO_ENABLE_ALSA)
    target_link_libraries(hsradio_hardware
        PRIVATE ALSA::ALSA
    )
endif()

target_compile_options(hsradio_hardware PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

if(HSRADIO_BUILD_TESTS)
    add_executable(hsradio_manager_test test/main.cpp)

    target_link_libraries(hsradio_manager_test
        PRIVATE hsradio_hardware
    )

    target_compile_options(hsradio_manager_test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
endif()

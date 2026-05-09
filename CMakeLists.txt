cmake_minimum_required(VERSION 3.10)
project(PhotoBridge LANGUAGES CXX VERSION 1.0.0)

#设置C++20标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#收集所有源文件
file(GLOB_RECURSE SOURCES src/*.cpp)

#链接源文件
add_executable(PhotoBridge ${SOURCES})

#链接外部库
target_include_directories(PhotoBridge PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/httplib

)

find_package(Threads REQUIRED)
target_link_libraries(PhotoBridge PRIVATE Threads::Threads)


if(WIN32)
    target_compile_definitions(PhotoBridge PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )

    target_link_libraries(PhotoBridge PRIVATE ws2_32)
endif()

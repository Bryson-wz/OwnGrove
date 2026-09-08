cmake_minimum_required(VERSION 3.10)
project(OwnGrove LANGUAGES CXX VERSION 0.1.0)

set(OWNGROVE_VERSION "${PROJECT_VERSION}-alpha")
configure_file(
    ${CMAKE_SOURCE_DIR}/include/owngrove/Version.h.in
    ${CMAKE_BINARY_DIR}/generated/owngrove/Version.h
    @ONLY
)

#设置C++20标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#收集所有源文件
file(GLOB_RECURSE SOURCES src/*.cpp)

#链接源文件
add_executable(owngrove_hub ${SOURCES})
set_target_properties(owngrove_hub PROPERTIES OUTPUT_NAME "owngrove-hub")

#链接外部库
target_include_directories(owngrove_hub PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/httplib
    ${CMAKE_BINARY_DIR}/generated
)

find_package(Threads REQUIRED)
target_link_libraries(owngrove_hub PRIVATE Threads::Threads)


if(WIN32)
    target_compile_definitions(owngrove_hub PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )

    target_link_libraries(owngrove_hub PRIVATE ws2_32)
endif()

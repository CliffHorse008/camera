cmake_minimum_required(VERSION 3.16)

project(rtsp_h264_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)

add_library(rtsp_core
    rtsp_common.c
    rtsp_h264.c
)

add_library(rtsp_server_lib
    rtsp_server.c
)

add_library(rtsp_client_lib
    rtsp_client.c
)

target_include_directories(rtsp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(rtsp_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(rtsp_client_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

foreach(target rtsp_core rtsp_server_lib rtsp_client_lib)
    target_compile_options(${target} PRIVATE
        -O2
        -Wall
        -Wextra
        -pedantic
    )
endforeach()

target_link_libraries(rtsp_server_lib PUBLIC rtsp_core Threads::Threads)
target_link_libraries(rtsp_server_lib PUBLIC m)
target_link_libraries(rtsp_client_lib PUBLIC rtsp_core)

add_executable(rtsp_h264_server main_server.c)
target_link_libraries(rtsp_h264_server PRIVATE rtsp_server_lib)

add_executable(rtsp_h264_client_demo main_client_demo.c)
target_link_libraries(rtsp_h264_client_demo PRIVATE rtsp_client_lib)

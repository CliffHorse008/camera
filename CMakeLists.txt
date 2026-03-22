cmake_minimum_required(VERSION 3.16)

project(rtsp_h264_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(FFPROBE_EXECUTABLE ffprobe)
find_package(PkgConfig QUIET)

option(RTSP_ENABLE_FFMPEG_INPUT "Enable MP4 input support through ffmpeg/ffprobe tools" ON)
option(RTSP_ENABLE_LIBAV_AAC "Enable PCM to AAC conversion in client demo through libavcodec/libavutil/libswresample" ON)

set(RTSP_WITH_FFMPEG_INPUT OFF)
if(RTSP_ENABLE_FFMPEG_INPUT)
    if(FFMPEG_EXECUTABLE AND FFPROBE_EXECUTABLE)
        set(RTSP_WITH_FFMPEG_INPUT ON)
    else()
        message(STATUS "RTSP_ENABLE_FFMPEG_INPUT=ON but ffmpeg/ffprobe not found; MP4 input support disabled")
    endif()
else()
    message(STATUS "MP4 input support disabled by RTSP_ENABLE_FFMPEG_INPUT=OFF")
endif()

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

if(RTSP_WITH_FFMPEG_INPUT)
    target_compile_definitions(rtsp_core PUBLIC RTSP_HAS_FFMPEG_INPUT=1)
endif()

set(RTSP_WITH_LIBAV_AAC OFF)
if(RTSP_ENABLE_LIBAV_AAC)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavcodec libavutil libswresample)
        if(LIBAV_FOUND)
            set(RTSP_WITH_LIBAV_AAC ON)
        else()
            message(STATUS "RTSP_ENABLE_LIBAV_AAC=ON but libavcodec/libavutil/libswresample not found; client audio AAC disabled")
        endif()
    else()
        message(STATUS "RTSP_ENABLE_LIBAV_AAC=ON but pkg-config not found; client audio AAC disabled")
    endif()
else()
    message(STATUS "Client AAC support disabled by RTSP_ENABLE_LIBAV_AAC=OFF")
endif()

target_link_libraries(rtsp_server_lib PUBLIC rtsp_core Threads::Threads)
target_link_libraries(rtsp_server_lib PUBLIC m)
target_link_libraries(rtsp_client_lib PUBLIC rtsp_core)

add_executable(rtsp_h264_server main_server.c)
target_link_libraries(rtsp_h264_server PRIVATE rtsp_server_lib)

add_executable(rtsp_h264_client_demo
    main_client_demo.c
    rtsp_ts_muxer.c
)
target_link_libraries(rtsp_h264_client_demo PRIVATE rtsp_client_lib)
if(RTSP_WITH_LIBAV_AAC)
    target_compile_definitions(rtsp_h264_client_demo PRIVATE RTSP_HAS_LIBAV_AAC=1)
    target_link_libraries(rtsp_h264_client_demo PRIVATE PkgConfig::LIBAV)
endif()

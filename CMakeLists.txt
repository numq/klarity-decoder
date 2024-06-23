cmake_minimum_required(VERSION 3.24)
project(klarity_decoder VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 23)

add_library(klarity_decoder SHARED
        src/format.cpp
        src/frame.cpp
        src/media.cpp
        src/decoder.cpp
)

target_include_directories(klarity_decoder PRIVATE include)

# FFMPEG

SET(FFMPEG_INCLUDE_PATH "include/ffmpeg")
target_include_directories(klarity_decoder PRIVATE ${FFMPEG_INCLUDE_PATH})

SET(FFMPEG_INCLUDE_PATH "bin/ffmpeg")
target_link_directories(klarity_decoder PRIVATE ${FFMPEG_INCLUDE_PATH})

target_link_libraries(klarity_decoder PRIVATE
        avutil-57
        swscale-6
        swresample-4
        avcodec-59
        avformat-59
)
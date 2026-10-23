# crash_assettool: native helper of tools/audio.py (RSD6XMA -> WAV, Bink -> MP4) using the LGPL
# FFmpeg DLLs in thirdparty/ffmpeg (loaded at run time). Built by tools/audio.py on first use:
#   cmake -S tools/assettool -B build/assettool -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
# with the bundled llvm-mingw, or clang-cl with Visual Studio (tools/buildenv.py picks the toolchain).
cmake_minimum_required(VERSION 3.20)
project(crash_assettool CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(CRASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(CRASH_FFMPEG_DIR ${CRASH_ROOT}/thirdparty/ffmpeg)
if (NOT EXISTS ${CRASH_FFMPEG_DIR}/include/libavcodec/avcodec.h)
    message(FATAL_ERROR "thirdparty/ffmpeg (headers + DLLs) is required")
endif()

add_executable(crash_assettool main.cpp rsd.cpp bink.cpp mp4_writer.cpp ffmpeg_api.cpp)
target_include_directories(crash_assettool PRIVATE ${CRASH_FFMPEG_DIR}/include)
target_compile_definitions(crash_assettool PRIVATE ASSETTOOL_ROOT_DIR="${CRASH_ROOT}" NOMINMAX WIN32_LEAN_AND_MEAN
                           _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
if (MINGW)
    target_link_options(crash_assettool PRIVATE -municode -static)
else()
    target_link_options(crash_assettool PRIVATE /ENTRY:wmainCRTStartup)
endif()

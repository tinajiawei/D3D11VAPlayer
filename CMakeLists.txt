cmake_minimum_required(VERSION 3.24)
project(MediaEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# FFmpeg shared 开发包（bin/include/lib 三件套），默认指向工作区内的本地副本
set(FFMPEG_ROOT "${CMAKE_CURRENT_LIST_DIR}/ffmpeg-8.1.2-full_build-shared"
    CACHE PATH "FFmpeg shared dev package root (contains bin/include/lib)")

if(MSVC)
    add_compile_options(/W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX UNICODE _UNICODE)
endif()

# ---- 把运行所需的 FFmpeg DLL 拷到 exe 旁（src 与 tests 共用） ----
function(copy_ffmpeg_dlls target)
    set(_dlls
        ${FFMPEG_ROOT}/bin/avcodec-62.dll
        ${FFMPEG_ROOT}/bin/avformat-62.dll
        ${FFMPEG_ROOT}/bin/avutil-60.dll
        ${FFMPEG_ROOT}/bin/swresample-6.dll
        ${FFMPEG_ROOT}/bin/swscale-9.dll
    )
    foreach(_dll IN LISTS _dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
endfunction()
add_subdirectory(src)
add_subdirectory(tests)

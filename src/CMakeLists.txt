# ---- 把运行所需的 FFmpeg DLL 拷到 exe 旁 ----
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

# ---- 核心库：core + media（不依赖窗口/图形，可独立测试） ----
add_library(media_engine_core STATIC
    core/log.cpp
    core/clock.cpp
    core/av_utils.cpp
    media/packet_queue.cpp
    media/frame_queue.cpp
    media/media_source.cpp
    media/sw_decoder.cpp
    media/d3d11va_decoder.cpp
    media/decoder_factory.cpp
)
target_include_directories(media_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_directories(media_engine_core PUBLIC ${FFMPEG_ROOT}/lib)
target_include_directories(media_engine_core PUBLIC ${FFMPEG_ROOT}/include)
target_link_libraries(media_engine_core PUBLIC avcodec avformat avutil swresample swscale)

# ---- 完整引擎：core + media + sync + audio + render + player ----
add_library(media_engine STATIC
    sync/sync_engine.cpp
    audio/audio_resampler.cpp
    audio/audio_output.cpp
    render/d3d11_renderer.cpp
    player/media_player.cpp
)
target_include_directories(media_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(media_engine PUBLIC media_engine_core d3d11 dxgi d3dcompiler ole32 uuid mmdevapi)

# ---- 应用：Win32 宿主窗口 + ImGui 控制面板 ----
add_executable(media_player_app
    ui/host_window.cpp
    ui/control_panel.cpp
    ui/main.cpp
)
target_link_libraries(media_player_app PRIVATE media_engine imgui user32 gdi32 shell32 comdlg32)
copy_ffmpeg_dlls(media_player_app)

# ---- FFmpeg 链接冒烟测试（验证 GCC 生成的导入库与 MSVC 兼容） ----
add_executable(ffmpeg_smoke smoke/main.cpp)
target_include_directories(ffmpeg_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_directories(ffmpeg_smoke PRIVATE ${FFMPEG_ROOT}/lib)
target_include_directories(ffmpeg_smoke PRIVATE ${FFMPEG_ROOT}/include)
target_link_libraries(ffmpeg_smoke PRIVATE avcodec avformat avutil swresample swscale)
copy_ffmpeg_dlls(ffmpeg_smoke)

# ---- ImGui（源码直入 third_party/imgui） ----
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/imgui/imgui.cpp")
    message(WARNING "third_party/imgui 缺失：应用 UI 将无法编译，请先下载 ImGui 源码")
endif()
add_library(imgui STATIC
    ../third_party/imgui/imgui.cpp
    ../third_party/imgui/imgui_draw.cpp
    ../third_party/imgui/imgui_tables.cpp
    ../third_party/imgui/imgui_widgets.cpp
    ../third_party/imgui/backends/imgui_impl_win32.cpp
    ../third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ../third_party/imgui ../third_party/imgui/backends)
target_link_libraries(imgui PRIVATE d3d11 dxgi)

# ---- WASAPI 端点诊断探针（tools/wasapi_probe.cpp） ----
add_executable(wasapi_probe ../tools/wasapi_probe.cpp)
target_link_libraries(wasapi_probe PRIVATE ole32 uuid mmdevapi)

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
    media/decoder_factory.cpp
    render/headless_renderer.cpp
    audio/null_audio_sink.cpp
)

# ---- 解码器插件 DLL（输出到 exe 旁 plugin\1\，参考 RDCodec plugin 目录模式） ----
add_library(decoder_sw SHARED
    media/sw_decoder.cpp
    media/plugin_sw.cpp
    core/av_utils.cpp
    core/log.cpp
)
target_compile_definitions(decoder_sw PRIVATE ME_DECODER_PLUGIN_BUILD)
target_include_directories(decoder_sw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_directories(decoder_sw PRIVATE ${FFMPEG_ROOT}/lib)
target_include_directories(decoder_sw PRIVATE ${FFMPEG_ROOT}/include)
target_link_libraries(decoder_sw PRIVATE avcodec avutil)
set_target_properties(decoder_sw PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:media_player_app>/plugin/1")

add_library(decoder_d3d11va SHARED
    media/d3d11va_decoder.cpp
    media/plugin_d3d11va.cpp
    core/av_utils.cpp
    core/log.cpp
)
target_compile_definitions(decoder_d3d11va PRIVATE ME_DECODER_PLUGIN_BUILD)
target_include_directories(decoder_d3d11va PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_directories(decoder_d3d11va PRIVATE ${FFMPEG_ROOT}/lib)
target_include_directories(decoder_d3d11va PRIVATE ${FFMPEG_ROOT}/include)
target_link_libraries(decoder_d3d11va PRIVATE avcodec avutil)
set_target_properties(decoder_d3d11va PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:media_player_app>/plugin/1")
target_include_directories(media_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_directories(media_engine_core PUBLIC ${FFMPEG_ROOT}/lib)
target_include_directories(media_engine_core PUBLIC ${FFMPEG_ROOT}/include)
target_link_libraries(media_engine_core PUBLIC avcodec avformat avutil swresample swscale)

# ---- 完整引擎：core + media + sync + audio + render + player ----
add_library(media_engine SHARED
    sync/sync_engine.cpp
    audio/audio_resampler.cpp
    audio/audio_output.cpp
    render/d3d11_renderer.cpp
    player/media_player.cpp
    api/me_api.cpp
)
target_compile_definitions(media_engine PRIVATE ME_ENGINE_DLL_BUILD)
target_include_directories(media_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(media_engine PRIVATE media_engine_core d3d11 dxgi d3dcompiler ole32 uuid mmdevapi)

# ---- 应用：Win32 宿主窗口 + ImGui 控制面板 ----
add_executable(media_player_app
    ui/host_window.cpp
    ui/control_panel.cpp
    ui/panel_window.cpp
    ui/floating_panel.cpp
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

add_executable(media_engine_tests
    test_main.cpp
    test_ring_buffer.cpp
    test_clock.cpp
    test_queues.cpp
    test_frame_queue.cpp
    test_stubs.cpp
)
target_link_libraries(media_engine_tests PRIVATE media_engine_core)
copy_ffmpeg_dlls(media_engine_tests)

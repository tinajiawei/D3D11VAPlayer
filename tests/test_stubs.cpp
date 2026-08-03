#include <chrono>
#include <thread>

#include "test_common.h"
#include "audio/null_audio_sink.h"
#include "render/headless_renderer.h"

void test_headless_renderer() {
    me::HeadlessRenderer r;
    CHECK(r.init(nullptr, 1920, 1080).ok());
    CHECK(r.is_ready());
    CHECK(r.device() == nullptr);
    CHECK(r.context() == nullptr);
    CHECK_EQ(r.width(), 1920);
    CHECK_EQ(r.height(), 1080);
    CHECK(r.draw_frame(nullptr).ok());
    CHECK(r.draw_frame(nullptr).ok());
    CHECK(r.present_swapchain().ok());
    CHECK_EQ(r.draw_count(), 2ULL);
    CHECK_EQ(r.present_count(), 1ULL);
    r.set_frame_rotation(90);
    CHECK_EQ(r.frame_rotation(), 90);
    r.set_pending_size(640, 480);
    CHECK_EQ(r.width(), 640);
    CHECK_EQ(r.height(), 480);
}

void test_null_audio_sink() {
    me::NullAudioSink s;
    CHECK(s.init(48000, 2, 0.2).ok());
    CHECK(s.start().ok());
    CHECK(s.is_active());

    float buf[4800] = {};
    s.write(buf, 4800);
    CHECK_EQ(s.written_samples(), 4800ULL);
    const double written = s.get_written_seconds();
    CHECK(written > 0.099 && written < 0.101);

    const double played0 = s.get_played_seconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const double played1 = s.get_played_seconds();
    CHECK(played1 > played0 + 0.02);

    // 暂停时主时钟冻结
    s.pause_stream();
    const double frozen = s.get_played_seconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(s.get_played_seconds() >= frozen - 0.002);
    CHECK(s.get_played_seconds() <= frozen + 0.002);
    s.resume_stream();

    s.clear_ring();
    s.reset_stream();
    CHECK_EQ(s.reset_count(), 1);
    CHECK_EQ(s.device_names().size(), 1ULL);
    CHECK(s.device_name() == "NullAudioSink");
    CHECK(s.switch_device(0).ok());
    CHECK_EQ(s.sample_rate(), 48000);
    CHECK_EQ(s.channels(), 2);

    s.stop();
    CHECK(!s.is_active());
}
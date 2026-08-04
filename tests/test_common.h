#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

inline int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto _a = (a);                                                          \
        auto _b = (b);                                                          \
        if (!(_a == _b)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d  %s == %s (%lld vs %lld)\n",       \
                         __FILE__, __LINE__, #a, #b,                            \
                         static_cast<long long>(_a), static_cast<long long>(_b)); \
            ++g_failures;                                                       \
        }                                                                      \
    } while (0)

void test_ring_buffer();
void test_clock();
void test_queues();
void test_frame_queue();
void test_headless_renderer();
void test_null_audio_sink();
void test_sequence();

inline int run_all_tests() {
    test_ring_buffer();
    test_clock();
    test_queues();
    test_frame_queue();
    test_headless_renderer();
    test_null_audio_sink();
    test_sequence();
    if (g_failures == 0) {
        std::fprintf(stderr, "ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", g_failures);
    return 1;
}

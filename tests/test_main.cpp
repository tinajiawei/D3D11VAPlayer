#include "test_common.h"

#include <cstring>
#include <cstdio>

int main(int argc, char** argv) {
    std::fprintf(stderr, "MAIN ENTERED\n");
    std::fflush(stderr);
    const bool only_ring = argc > 1 && std::strcmp(argv[1], "ring") == 0;
    const bool only_clock = argc > 1 && std::strcmp(argv[1], "clock") == 0;
    const bool only_queues = argc > 1 && std::strcmp(argv[1], "queues") == 0;

    if (!only_clock && !only_queues) { std::fprintf(stderr, "RUN ring\n"); std::fflush(stderr); test_ring_buffer(); }
    if (!only_ring && !only_queues) { std::fprintf(stderr, "RUN clock\n"); std::fflush(stderr); test_clock(); }
    if (!only_ring && !only_clock)  { std::fprintf(stderr, "RUN queues\n"); std::fflush(stderr); test_queues(); }
    if (!only_ring && !only_clock)  { test_frame_queue(); }

    std::fprintf(stderr, "DONE\n");
    std::fflush(stderr);
    return g_failures == 0 ? 0 : 1;
}

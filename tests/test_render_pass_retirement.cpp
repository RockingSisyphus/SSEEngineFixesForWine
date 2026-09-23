#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>
#include <vector>

#include "../src/patches/render_pass_retirement.h"

namespace {
    struct Pass {
        std::uint32_t pad44 = 0;
        bool freed = false;
    };
    using Queue = Patches::RenderPassCache::RetirementQueue<Pass>;
    void release(Pass* pass) { pass->freed = true; }
}

TEST_CASE("Render passes remain alive for three frames", "[renderpass]")
{
    Queue queue;
    Pass passes[3];
    std::uint32_t frame = 100;
    auto clock = [&] { return frame; };

    queue.Retire(&passes[0], clock, release);
    queue.Retire(&passes[0], clock, release);
    frame = 102;
    queue.Retire(&passes[1], clock, release);
    REQUIRE_FALSE(passes[0].freed);
    frame = 103;
    queue.Retire(&passes[2], clock, release);
    REQUIRE(passes[0].freed);
    REQUIRE_FALSE(passes[1].freed);
    REQUIRE(queue.Snapshot().duplicateRetires == 1);
}

TEST_CASE("Frame resets and wraparound do not free passes early", "[renderpass]")
{
    Queue queue;
    Pass passes[4];
    std::uint32_t frame = 100;
    auto clock = [&] { return frame; };

    queue.Retire(&passes[0], clock, release);
    frame = 0;
    queue.Retire(&passes[1], clock, release);
    REQUIRE_FALSE(passes[0].freed);
    frame = 2;
    queue.Retire(&passes[2], clock, release);
    REQUIRE_FALSE(passes[0].freed);
    frame = 3;
    queue.Retire(&passes[3], clock, release);
    REQUIRE(passes[0].freed);

    Queue wrapQueue;
    Pass wrapped[3];
    frame = 0xFFFFFFFEu;
    wrapQueue.Retire(&wrapped[0], clock, release);
    frame = 0;
    wrapQueue.Retire(&wrapped[1], clock, release);
    REQUIRE_FALSE(wrapped[0].freed);
    frame = 1;
    wrapQueue.Retire(&wrapped[2], clock, release);
    REQUIRE(wrapped[0].freed);
}

TEST_CASE("Frozen frame clock retains passes under concurrent retirement", "[renderpass]")
{
    Queue queue;
    std::vector<Pass> passes(32001);
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 32; ++thread) {
        workers.emplace_back([&, thread] {
            for (int i = thread * 1000; i < (thread + 1) * 1000; ++i) {
                queue.Retire(&passes[i], [] { return 7u; }, release);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    REQUIRE(queue.Snapshot().pending == 32000);
    REQUIRE(queue.Snapshot().freed == 0);
    for (int i = 0; i < 32000; ++i) REQUIRE_FALSE(passes[i].freed);

    queue.Retire(&passes.back(), [] { return 10u; }, release);
    REQUIRE(queue.Snapshot().freed == 32000);
    REQUIRE(queue.Snapshot().pending == 1);
}

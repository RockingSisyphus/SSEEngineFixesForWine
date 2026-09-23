#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace Patches::RenderPassCache
{
    // Based on alandtse/EngineFixesSkyrim64's deferred-free policy (MIT).
    // Keep retired passes intact for three engine frames. Unlike its bounded
    // ring, capacity pressure never overrides the minimum retirement age.
    template <class Pass>
    class RetirementQueue
    {
    public:
        static constexpr std::uint32_t retiredTag = 0xD1ED0FF5;
        static constexpr std::uint32_t framesToKeep = 3;
        struct Stats {
            std::uint64_t retired, freed, duplicateRetires;
            std::size_t pending, peak;
        };

        template <class Clock, class Free>
        void Retire(Pass* pass, Clock clock, Free free)
        {
            if (!pass) return;
            std::lock_guard lock(mutex);
            if (pass->pad44 == retiredTag) {
                ++duplicateRetires;
                return;
            }
            // Read the frame after acquiring the lock: FIFO timestamps must
            // not move backwards when worker threads acquire it out of order.
            const std::uint32_t now = clock();
            while (!queue.empty()) {
                const auto age = now - queue.front().frame;
                // A reset of the frame clock retains objects rather than
                // interpreting a backwards jump as billions of elapsed frames.
                if (age >= 0x80000000u) {
                    for (auto& item : queue) item.frame = now;
                    break;
                }
                if (age < framesToKeep) break;
                free(queue.front().pass);
                queue.pop_front();
                ++freed;
            }
            queue.push_back({ pass, now });
            pass->pad44 = retiredTag;
            ++retired;
            if (queue.size() > peak) peak = queue.size();
        }

        Stats Snapshot()
        {
            std::lock_guard lock(mutex);
            return { retired, freed, duplicateRetires, queue.size(), peak };
        }

    private:
        struct Entry { Pass* pass; std::uint32_t frame; };
        std::mutex mutex;
        std::deque<Entry> queue;
        std::uint64_t retired = 0, freed = 0, duplicateRetires = 0;
        std::size_t peak = 0;
    };
}

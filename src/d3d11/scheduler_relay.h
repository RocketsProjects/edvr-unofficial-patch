#pragma once
#include <cstddef>
#include <cstdint>

// Private first-party boundary. The record and context are immutable and
// process-lived while registered. The graphics module is pinned before any
// engine hook is installed, so a worker's detached snapshot remains valid.
struct EdvrSchedulerRelayObserverOps {
    uint32_t structSize;
    uint32_t version;
    void* context;
    void (*feed)(void* context, uint32_t target, uint64_t entryRsp);
};
static_assert(sizeof(EdvrSchedulerRelayObserverOps) == 24,
              "x64 scheduler observer ABI must remain fixed");
static_assert(offsetof(EdvrSchedulerRelayObserverOps, feed) == 16,
              "scheduler callback follows opaque context");

extern "C" {
const char* edvrKinematicEvalSchedulerObserverAttach(
    const EdvrSchedulerRelayObserverOps* ops) noexcept;
void edvrKinematicEvalSchedulerObserverDetach(
    const EdvrSchedulerRelayObserverOps* exactOps) noexcept;
}

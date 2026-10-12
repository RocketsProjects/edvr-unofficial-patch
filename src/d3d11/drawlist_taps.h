#pragma once
// The draw-list probe's tap points (TEMPORARY, Built v9). The engine's existing observers and the substituted-draw counter call the inline
// wrappers below. They reach the probe only through a function table the probe installs when it is armed, so this header and drawlist_taps.cpp
// depend on nothing: the rigs that link engine_velocity.cpp need only drawlist_taps.cpp. Off (no window collecting), each tap is one atomic load.
#include <atomic>
#include <cstdint>

namespace edvr {

struct DrawlistTapFns {
    void (*builder)(uintptr_t rec, uintptr_t instance);                                   // path A (the builder bracket observer)
    void (*emit)(uintptr_t record, uintptr_t owner, int32_t before, int32_t after);       // path B (the producer's emit observer)
    void (*copy)(uintptr_t mapped, uint32_t count);                                       // the pool copy observer
    void (*merge)(uintptr_t destination);                                                 // the merge observer
    void (*clear)(uintptr_t dictionary);                                                  // the dictionary clear observer
    void (*family)(int family);                                                           // a substituted pool draw of one family
    void (*producerC)(uint64_t items, uint32_t fault);                                    // Built v11: path C (FUN_14369C9C0, producer 1), after its forward
};

// True while a window is collecting (set by the probe at the window's first tick, cleared at its last).
extern std::atomic<bool> g_drawlistCollecting;
// The probe's table, installed when it is armed. Null until then.
extern std::atomic<const DrawlistTapFns*> g_drawlistTaps;

inline void drawlistNoteBuilder(uintptr_t rec, uintptr_t instance) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->builder(rec, instance);
}
inline void drawlistNoteEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->emit(record, owner, before, after);
}
inline void drawlistNoteCopy(uintptr_t mapped, uint32_t count) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->copy(mapped, count);
}
inline void drawlistNoteMerge(uintptr_t destination) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->merge(destination);
}
inline void drawlistNoteClear(uintptr_t dictionary) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->clear(dictionary);
}
inline void drawlistNoteProducerC(uint64_t items, uint32_t fault) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->producerC(items, fault);
}
inline void drawlistNoteFamily(int family) noexcept {
    if (!g_drawlistCollecting.load(std::memory_order_acquire)) return;
    if (const DrawlistTapFns* t = g_drawlistTaps.load(std::memory_order_acquire)) t->family(family);
}

}  // namespace edvr

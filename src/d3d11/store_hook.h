#pragma once
// The admission store's detour (FUN_144312040; TEMPORARY, shared by the sealed-hangar trial and the look probe).
//
// The store writes each record's view mask (rec+0x208) and admitted flag (rec+0x234), then recurses into the record's child
// list. This detour forwards the call (five arguments: the bucket, the mask, the output, the LOD, and the record on the
// stack) and then lets its consumers look at the record. With no consumer wanting it, the gate is closed and the relay goes
// straight to the original: nothing is observed and nothing is written.
//
// Installed on the first want (the trial's toggle or the look probe's press), never at launch.
#include <cstdint>

namespace edvr {

// A consumer says whether it wants the store observed (the sealed trial on, or the look probe collecting). The detour
// is installed on the first true; the gate follows the union of the consumers.
void storeHookRecompute() noexcept;
// The detour's status, for the consumers' lines ("installed", or why not).
const char* storeHookStatus() noexcept;

}  // namespace edvr

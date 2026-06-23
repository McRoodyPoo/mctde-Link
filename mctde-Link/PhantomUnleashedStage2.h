/*
    PhantomUnleashedStage2  -  mctde-Link (built into d3d9.dll)

    Stage 2 of the phantom-cap raise: the offset-shift trampolines, code caves, and
    deferred AoB writes that make a >4 session actually stable. See PhantomUnleashedStage2.cpp.

    A rewrite of the MultiPhantom offset-shift trampoline strategy using mctde-Link's
    reversible patch framework. The trampoline sites, shifted field offsets, and AoB
    patterns are derived from Metal-Crow's reverse engineering / Dark Souls Overhaul work.
*/
#pragma once

#include <cstdint>

namespace mp {

// Install the Stage 2 trampolines + caves + patch35 and spawn the deferred AoB worker.
// Calibrated for maxPhantoms == 18; any other value logs a warning and skips (Stage 1
// still segregates/caps, but the array layout is not stable). Call only after Stage 1
// has committed and only when VerifyOnly=0. Reversible.
void PhantomUnleashedStage2_Install(uint8_t maxPhantoms);

// Revert every Stage 2 site. Idempotent; safe from teardown / watchdog paths.
void PhantomUnleashedStage2_Restore();

} // namespace mp

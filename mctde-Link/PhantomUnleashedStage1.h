/*
    PhantomUnleashedStage1  -  mctde-Link (built into d3d9.dll)

    Raises PTDE's simultaneous-phantom cap above the stock 4 (Stage 1: the static,
    count-parameterized byte patches + the matchmaking-pool version, with verification).

    Based on Metal-Crow's reverse-engineered MultiPhantom / Dark Souls Overhaul patch map
    and behavior. This is a rewrite for mctde-Link's patch engine -- parameterization,
    verification, logging, and reversible restore. No source text is intentionally copied;
    the RVAs / opcodes / offset facts are credited to the original reverse-engineering work.
*/
#pragma once

#include <cstdint>

namespace mp {

struct PhantomUnleashedConfig {
    uint8_t maxPhantoms = 18; // total player slots in your world. Stock is 4.
                             // 4 == behavioural no-op (use it to validate the engine
                             // against the live game before raising the cap).

    // Matchmaking pool key. The game only pairs players whose 1-byte network version
    // matches. Vanilla retail PTDE = 0x2E. Setting a PhantomUnleashed-specific value puts
    // you in a pool with ONLY other players sharing the same value (segregation).
    // Set to 0x2E to stay in the vanilla pool (no segregation).
    uint8_t networkVersion = 0x4D; // 'M' for PhantomUnleashed; != vanilla, Overhaul, Rekindled.

    // Game internal memory-pool size, in bytes. Stock is 0xA20000 (~10 MB), sized for 4
    // players; too small once many phantoms load their models/gear/animations. 0 leaves it
    // stock. Values above the stock size patch the three pool-allocation sites; the engine
    // clamps to the safe max 0x0FFFFFFF (>= 0x1FFFFFFF crashes the game).
    uint32_t memoryPoolBytes = 0;
};

// Resolve ds1_base (the DARKSOULS.exe module base) and stage every static patch.
// Returns false if the base can't be resolved or maxPhantoms is out of range.
bool PhantomUnleashedStage1_Prepare(const PhantomUnleashedConfig& cfg);

// Dry run: log whether every site's opcode bytes match. Safe; changes nothing.
int  PhantomUnleashedStage1_Verify();

// Apply the staged static patches (Stage 1). NOTE: not a complete multiphantom on its
// own — Stage 2 trampolines are required before a >4 session is stable.
bool PhantomUnleashedStage1_Apply();

// Revert everything. Safe to call from DLL_PROCESS_DETACH / watchdog teardown.
void PhantomUnleashedStage1_Restore();

} // namespace mp

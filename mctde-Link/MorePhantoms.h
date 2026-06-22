/*
    MorePhantoms  -  mctde-Link (built into d3d9.dll)

    Host glue for the MorePhantoms patch modules (Stage 1 + Stage 2). See MorePhantoms.cpp.

    All three entry points run on the HubThread / teardown paths -- never inside
    DllMain -- so the modal prompt and the thread-freezing patcher are loader-lock safe.
*/
#pragma once

// Launch-time Yes/No opt-in. Call early on HubThread, BEFORE StartBuiltInModules()
// applies anything and before any online/session init. Honours [MorePhantoms] Mode
// (Ask shows the box; On/Off skip it) and records the answer for this session.
void MorePhantoms_Prompt();

// Prepare/Verify/Apply per [MorePhantoms] ini + the prompt result. Call from
// StartBuiltInModules(). No-op if the player did not enable MorePhantoms.
void MorePhantoms_Start();

// Revert every applied patch. Idempotent; safe to call repeatedly from
// OnProcessDetach and the watchdog teardown path.
void MorePhantoms_Restore();

// Total player-slot count currently live in the game's session arrays: the applied
// MaxPhantoms when MorePhantoms has committed its patches, otherwise the stock 4.
// The overlay uses this to know how many phantom slots to read from the world.
int MorePhantoms_ActivePhantomCount();

/*
    HideSoulCounter  -  mctde-Link (built into d3d9.dll)

    Reversibly blanks the bottom-right HUD soul-counter NUMBER. The soul icon and the
    rest of the HUD are untouched -- only the digits stop being drawn.

    Mechanism (see HideSoulCounter.cpp for the full write-up): the in-game F20 HUD's
    "soul_param" widget update method formats the rolling soul total into the
    "text_param_soul" text element only when the value changed since last frame -- guarded
    by `cmp [esi+0x104], edi ; jz <skip>`. We flip that JZ to an unconditional JMP, so the
    game always takes the existing "unchanged" path it already runs ~every frame. The digit
    text element is then never populated. One reversible byte (0x74 -> 0xEB), located by a
    unique AoB so it survives relocation.
*/
#pragma once

// Gate from Direct3DCreate9 on the game's main thread (idempotent). Reads the
// [HideSoulCounter] ini section; VerifyOnly by default until the player opts in.
void HideSoulCounter_Start();

// Revert the patch on teardown (idempotent).
void HideSoulCounter_Restore();

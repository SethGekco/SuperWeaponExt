#pragma once

// SuperWeaponExt — weapon-choice override ("use a different weapon against a
// designator/inhibitor"). Implementation and the full design notes live in
// Hooks.WeaponSelect.cpp.
//
// The wrapper that does the work runs on PerfExt's worker threads as well as
// the game thread, so it is strictly read-only. Everything it reads is produced
// by FrameTick below, which must be called exactly once per frame from the
// shared per-frame tick in Hooks.ParaDrop.cpp — on the game thread, after the
// object-update loop.
namespace SWExt::WeaponSelect
{
    void FrameTick();
}

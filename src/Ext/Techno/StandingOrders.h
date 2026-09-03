#pragma once
/*
 * SuperWeaponExt — unit standing orders (engine adapter interface).
 *
 * Deliberately its own header rather than more surface on SWTypeExt: the
 * blackboard is written by the superweapon launch path but read by the techno
 * tick, and keeping the seam explicit makes the one-way dependency obvious.
 */
#include <CellClass.h>

class HouseClass;

namespace SWExt::StandingOrders
{
    // Record where a superweapon just landed for `pHouse`. Called from the
    // Fire_SW hook, the one place every launch is guaranteed to pass.
    void RecordImpact(HouseClass* pHouse, const CellStruct& cell);

    // Drop every recorded impact. Called on scenario start so a new match never
    // inherits the previous one's blackboard.
    void ClearImpacts();

    // Per-frame evaluation. Called from the LogicClass::AI seat we already own
    // in Hooks.ParaDrop.cpp — see the comment there for why this is not its own
    // DEFINE_HOOK.
    void Tick();
}

// Included by Hooks.Launch.cpp — beacon placement on superweapon launch.
//
// WHY THERE IS NO CUSTOM COMMAND AND NO CUSTOM EVENT.
//
// The obvious build is a CommandClass that arms a cursor, captures a click and
// queues a network event carrying the cell. Every one of those pieces already
// exists on a superweapon, and reusing them is not merely cheaper -- it is more
// correct:
//
//   targeting   the SW cursor already picks a cell, under the engine's own
//               range and shroud rules
//   networking  Fire_SW runs on EVERY client with the SAME cell, so the spawn
//               is synced by construction. No custom event, and none of the
//               desync exposure CommandBarExt's locally-placed no-go zones
//               still carry.
//   cooldown    the recharge clock
//   charges     SW charges
//   sidebar     SW.ShowCameo, plus SWExt.HotkeyIndex for a dedicated key that
//               works with no cameo at all
//   bar button  that hotkey is a registered CommandClass, so CommandBarExt can
//               surface it BY NAME with no linkage between the two DLLs
//
// So "the beacon placement command" is a rules tag on a superweapon, and the
// command half already shipped.
//
// The beacon itself is an ordinary techno (see SW/Beacon.h), which is what
// makes weapons able to target it and makes our OWN designator/inhibitor scan
// see it with no new code.
//
// WARNING: runs on every client. Do not guard any of this on CurrentPlayer and
// do not queue an event -- either would make clients disagree about the map.

namespace
{
    // Remove this house's existing beacons of the same type, so Replace=yes
    // means "move my beacon" rather than "litter the map".
    //
    // Iterates DOWNWARD: UnInit() removes the techno from TechnoClass::Array,
    // so an upward loop would skip whatever gets shuffled into the freed slot.
    void RemoveExistingBeacons(HouseClass* pOwner, TechnoTypeClass* pBeaconType)
    {
        for (int i = TechnoClass::Array.Count - 1; i >= 0; --i)
        {
            TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
            if (!pTechno || pTechno->Owner != pOwner)
                continue;
            if (pTechno->GetTechnoType() != pBeaconType)
                continue;

            pTechno->Limbo();
            pTechno->UnInit();
        }
    }

    void PlaceBeacon(SWTypeExt::ExtData* pExt, HouseClass* pOwner,
                     const CellStruct& cell)
    {
        if (!pExt || !pExt->BeaconSpawns || !pOwner)
            return;

        auto const pType = pExt->BeaconSpawns;

        if (pExt->BeaconReplace)
            RemoveExistingBeacons(pOwner, pType);

        auto const pNew = pType->CreateObject(pOwner);
        if (!pNew)
            return;

        if (!pNew->Unlimbo(CellClass::Cell2Coord(cell), DirType::North))
        {
            // Occupied or off-map. Free it rather than leaking a limboed
            // object into the heap for the rest of the match.
            pNew->UnInit();
            Debug::Log("[SuperWeaponExt] beacon %s could not be placed at "
                       "(%d,%d) for house %d\n", pType->ID, cell.X, cell.Y,
                       pOwner->ArrayIndex);
            return;
        }

        SWTypeExt::RegisterBeacon(abstract_cast<TechnoClass*>(pNew),
                                  pExt->BeaconLifetime);

        Debug::Log("[SuperWeaponExt] beacon %s placed at (%d,%d) for house %d "
                   "(lifetime %d)\n", pType->ID, cell.X, cell.Y,
                   pOwner->ArrayIndex, pExt->BeaconLifetime);
    }
}

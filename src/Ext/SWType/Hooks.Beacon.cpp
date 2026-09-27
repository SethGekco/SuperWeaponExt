/*
 * SuperWeaponExt — beacon lifetime and unit response.
 *
 * Placement lives in Hooks.Launch.cpp (via SW/BeaconPlacement.inc): the
 * superweapon IS the placement mechanism, so targeting, the network event, the
 * recharge clock and the charge count all come for free, and Fire_SW runs on
 * every client with the same cell so the spawn is synced by construction.
 *
 * This file owns what happens AFTERWARDS: expiry, and making units react.
 *
 * ============================================================================
 * THE THREE RESPONSES, and why they are actually different
 * ============================================================================
 *
 * Mission::Area_Guard is the engaging mission — it is what the engine itself
 * uses for guard-area and attack-move (confirmed against Phobos'
 * Ext/Script/Body.cpp:242, which queues exactly this for a guard-area script
 * action). Area_Guard PLUS a destination is therefore attack-move: walk there,
 * shoot what you meet. That single fact is what lets these three modes be
 * genuinely distinct rather than three names for one behaviour:
 *
 *   inrangeonly  Area_Guard with NO destination, and only for units already
 *                inside the radius. They hold position and engage what comes
 *                to them. Nobody is dragged across the map.
 *   attackmove   Area_Guard + destination = the beacon, but only for units that
 *                are IDLE. A busy unit keeps its orders; the beacon is a
 *                suggestion to whoever is free.
 *   aggressive   the same, but re-tasks units whether or not they are idle. The
 *                beacon outranks whatever the unit was doing.
 *
 * ⚠ DETERMINISM. Every input here is synced simulation state: the frame
 * counter, TechnoClass::Array order, object coordinates and house ownership.
 * There is no cursor, no local view and no unsynced randomness, so every client
 * issues the identical orders on the identical frame. The missions are queued
 * directly rather than as network events for the same reason AutoFire fires
 * directly — see SW/AutoFire.h: queueing would have each of N clients enqueue
 * the same order and produce N of them.
 *
 * ⚠ POINTER SAFETY. A dead techno's memory is pooled and reused, so IsAlive
 * cannot be trusted on a pointer we merely remembered. Anything held across
 * frames is validated by membership in TechnoClass::Array first. Same rule as
 * CommandBarExt's anchored no-go zones.
 */
#include "Body.h"

#include <Ext/TechnoType/Body.h>

#include <CellClass.h>
#include <FootClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Utilities/Debug.h>
#include <Helpers/Cast.h>

#include <vector>

namespace
{
    struct TrackedBeacon
    {
        TechnoClass* Beacon;
        int          ExpiryFrame;   // <0 = never
        int          SWTypeIndex;   // which superweapon's rules govern it
    };

    std::vector<TrackedBeacon> g_beacons;

    // Re-issuing orders every frame would jitter units in place and burn CPU
    // for no gain, so responses are evaluated on a coarse cadence. 15 frames is
    // about a second at normal speed — responsive without thrashing.
    constexpr int ResponseInterval = 15;

    bool StillAlive(TechnoClass* pTechno)
    {
        for (int i = 0; i < TechnoClass::Array.Count; ++i)
        {
            if (TechnoClass::Array.GetItem(i) == pTechno)
                return true;
        }
        return false;
    }

    bool TypeListed(const std::vector<int>& list, TechnoTypeClass* pType)
    {
        if (list.empty())
            return true;   // empty = every unit the house owns

        const int idx = TechnoTypeExt::UnifiedIndex(pType);
        for (int wanted : list)
            if (wanted == idx)
                return true;
        return false;
    }

    // An idle unit: nothing queued, no destination, no target. Deliberately
    // conservative -- if in doubt, treat it as busy and leave it alone.
    bool IsIdleEnough(FootClass* pFoot)
    {
        return !pFoot->Destination && !pFoot->Target
            && pFoot->CurrentMission != Mission::Attack
            && pFoot->CurrentMission != Mission::Move;
    }

    void ApplyResponse(const TrackedBeacon& tracked, SWTypeExt::ExtData* pExt)
    {
        if (!pExt || pExt->BeaconResponse == SWExt::BeaconUnitResponse::None)
            return;

        TechnoClass* const pBeacon = tracked.Beacon;
        HouseClass* const pOwner = pBeacon->Owner;
        if (!pOwner)
            return;

        CellStruct beaconCell{};
        pBeacon->GetMapCoords(&beaconCell);

        const int notice = pExt->BeaconNoticeRange >= 0
            ? pExt->BeaconNoticeRange
            : pExt->BeaconRange;
        const int noticeSq = notice * notice;

        CellClass* const pDestCell = MapClass::Instance.TryGetCellAt(beaconCell);
        if (!pDestCell)
            return;

        const bool inRangeOnly =
            pExt->BeaconResponse == SWExt::BeaconUnitResponse::InRangeOnly;
        const bool aggressive =
            pExt->BeaconResponse == SWExt::BeaconUnitResponse::Aggressive;

        const int radiusSq = pExt->BeaconRange * pExt->BeaconRange;
        int ordered = 0;

        for (int i = 0; i < TechnoClass::Array.Count; ++i)
        {
            TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
            if (!pTechno || pTechno == pBeacon || pTechno->Owner != pOwner)
                continue;
            if (pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Health)
                continue;

            // Only FootClass moves; buildings cannot be ordered anywhere.
            auto const pFoot = abstract_cast<FootClass*>(pTechno);
            if (!pFoot)
                continue;

            TechnoTypeClass* const pType = pTechno->GetTechnoType();
            if (!pType || !TypeListed(pExt->BeaconUnits, pType))
                continue;

            CellStruct here{};
            pTechno->GetMapCoords(&here);
            const int dx = here.X - beaconCell.X;
            const int dy = here.Y - beaconCell.Y;
            const int distSq = dx * dx + dy * dy;

            if (inRangeOnly)
            {
                // Hold position and engage. No destination, so nobody is
                // dragged in from elsewhere.
                if (distSq > radiusSq)
                    continue;

                pFoot->QueueMission(Mission::Area_Guard, false);
                ++ordered;
                continue;
            }

            if (distSq > noticeSq)
                continue;   // too far away to have noticed it

            if (!aggressive && !IsIdleEnough(pFoot))
                continue;   // attackmove only re-tasks the free

            // Already standing on it -- re-issuing would jitter it in place.
            if (here.X == beaconCell.X && here.Y == beaconCell.Y)
                continue;

            // Mission FIRST, then destination: the engine's own idiom for a
            // scripted move (see Hooks.StandingOrder.cpp for the full note).
            // Reversing it lets the mission change clear the destination.
            pFoot->QueueMission(Mission::Area_Guard, false);
            pFoot->SetDestination(pDestCell, true);
            ++ordered;
        }

        if (ordered > 0)
        {
            Debug::Log("[SuperWeaponExt] beacon response: %d unit(s) -> "
                       "(%d,%d) mode %d\n", ordered, beaconCell.X,
                       beaconCell.Y, static_cast<int>(pExt->BeaconResponse));
        }
    }
}

void SWTypeExt::RegisterBeacon(TechnoClass* pBeacon, int lifetime, int swTypeIndex)
{
    if (!pBeacon)
        return;

    g_beacons.push_back(TrackedBeacon{
        pBeacon,
        lifetime < 0 ? -1 : Unsorted::CurrentFrame + lifetime,
        swTypeIndex });
}

void SWTypeExt::ClearBeacons()
{
    g_beacons.clear();
}

void SWTypeExt::TickBeacons()
{
    if (g_beacons.empty())
        return;

    const int now = Unsorted::CurrentFrame;

    for (int i = static_cast<int>(g_beacons.size()) - 1; i >= 0; --i)
    {
        TechnoClass* const pBeacon = g_beacons[i].Beacon;

        if (!StillAlive(pBeacon))
        {
            // Destroyed by other means; just stop tracking it.
            g_beacons.erase(g_beacons.begin() + i);
            continue;
        }

        if (g_beacons[i].ExpiryFrame >= 0 && now >= g_beacons[i].ExpiryFrame)
        {
            Debug::Log("[SuperWeaponExt] beacon expired for house %d\n",
                       pBeacon->Owner ? pBeacon->Owner->ArrayIndex : -1);

            pBeacon->Limbo();
            pBeacon->UnInit();
            g_beacons.erase(g_beacons.begin() + i);
            continue;
        }

        // Stagger by index so many beacons never all evaluate on one frame.
        if (((now + i) % ResponseInterval) != 0)
            continue;

        auto const pSWType =
            SuperWeaponTypeClass::Array.GetItemOrDefault(g_beacons[i].SWTypeIndex);
        if (!pSWType)
            continue;

        ApplyResponse(g_beacons[i], SWTypeExt::ExtMap.Find(pSWType));
    }
}

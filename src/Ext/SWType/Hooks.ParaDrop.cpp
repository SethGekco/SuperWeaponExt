/*
 * SuperWeaponExt — owned paradrop.
 *
 * ============================================================================
 * WHY WE OWN IT RATHER THAN EXTEND IT
 * ============================================================================
 *
 * Antares performs paradrops in SW_ParaDrop::SendPDPlane
 * (Antares src/Misc/SWTypes/ParaDrop.cpp:292). That function CONSTRUCTS the
 * aircraft itself, so there is no engine address to intercept — the veto-layer
 * trick that works for inhibitors has nothing to bite on here. And its entire
 * spawn-position decision is eight lines:
 *
 *     auto edge = pOwner->StartingEdge;            // fallback pOwner->Edge, then North
 *     auto const spawn_cell = MapClass::Instance.PickCellOnEdge(edge, ...);
 *
 * Every plane in a multi-plane drop calls that independently, so each picks its
 * own RANDOM cell on the same edge. That is the "they spawn randomly all around"
 * behaviour, and it is not configurable from outside Antares.
 *
 * So: a superweapon with `SWExt.ParaDrop=yes` is handled entirely here. Our
 * Layer 1 Fire_SW hook runs BEFORE anything Antares does for that launch (its
 * paradrop happens further down, inside SuperClass::Launch), so we do the drop
 * and then abort the normal path. Antares' copy never executes for that shot.
 * No address contention at all.
 *
 * ============================================================================
 * Determinism
 * ============================================================================
 *
 * Everything here is driven from the SpecialPlace event via Fire_SW, so it runs
 * on the same frame on every client with the same inputs. The pending queue is
 * keyed off Unsorted::CurrentFrame for the same reason.
 *
 * ⚠ KNOWN LIMITATION: the pending queue is NOT serialized. Saving mid-drop and
 * reloading loses any planes that had not launched yet. That is a deterministic
 * loss — every client loses exactly the same entries — so it cannot desync, but
 * it is a real behavioural gap worth fixing if delayed drops become load-bearing.
 */
#include "Body.h"

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <CellClass.h>
#include <Fundamentals.h>   // Unsorted::CurrentFrame
#include <HouseClass.h>
#include <MapClass.h>
#include <TechnoTypeClass.h>
#include <Unsorted.h>       // Unsorted::ScenarioInit
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <vector>

namespace
{
    // One plane still waiting for its delay to elapse.
    struct PendingDrop
    {
        int                LaunchFrame = 0;
        AircraftTypeClass* Aircraft    = nullptr;
        HouseClass*        Owner       = nullptr;
        CellStruct         Target      {};
        Edge               Entry       = Edge::North;
        std::vector<TechnoTypeClass*> Types;
        std::vector<int>              Nums;
    };

    std::vector<PendingDrop> g_pending;

    Edge ToEngineEdge(SWExt::ApproachEdge e)
    {
        switch (e)
        {
        case SWExt::ApproachEdge::East:  return Edge::East;
        case SWExt::ApproachEdge::South: return Edge::South;
        case SWExt::ApproachEdge::West:  return Edge::West;
        default:                         return Edge::North;
        }
    }

    SWExt::ApproachEdge FromEngineEdge(Edge e)
    {
        switch (e)
        {
        case Edge::East:  return SWExt::ApproachEdge::East;
        case Edge::South: return SWExt::ApproachEdge::South;
        case Edge::West:  return SWExt::ApproachEdge::West;
        default:          return SWExt::ApproachEdge::North;
        }
    }

    // Resolve the configured origin into a concrete map edge.
    Edge ResolveEntryEdge(const ParaDropConfig& cfg, HouseClass* pOwner,
                          const CellStruct& target)
    {
        switch (cfg.Origin)
        {
        case ParaDropOrigin::North: return Edge::North;
        case ParaDropOrigin::East:  return Edge::East;
        case ParaDropOrigin::South: return Edge::South;
        case ParaDropOrigin::West:  return Edge::West;

        case ParaDropOrigin::Nearest:
        {
            // MapCoordBounds is the playable area in cells.
            auto const& b = MapClass::Instance.MapCoordBounds;
            return ToEngineEdge(SWExt::NearestEdge(
                target.X, target.Y, b.Left, b.Top, b.Right, b.Bottom));
        }

        case ParaDropOrigin::Owner:
        default:
        {
            // Mirrors Antares' fallback chain exactly: StartingEdge, then Edge,
            // then North — both can hold out-of-range values on some maps.
            Edge edge = pOwner ? pOwner->StartingEdge : Edge::North;
            if (edge < Edge::North || edge > Edge::West)
                edge = pOwner ? pOwner->Edge : Edge::North;
            if (edge < Edge::North || edge > Edge::West)
                edge = Edge::North;
            return edge;
        }
        }
    }

    // Build and launch ONE plane carrying `types`/`nums`, aimed at `target`.
    // Deliberately mirrors Antares' SendPDPlane so behaviour matches except for
    // the parts we are changing (entry edge, target cell).
    bool LaunchPlane(HouseClass* pOwner, AircraftTypeClass* pPlaneType,
                     const CellStruct& target, Edge entry,
                     const std::vector<TechnoTypeClass*>& types,
                     const std::vector<int>& nums)
    {
        if (!pOwner || !pPlaneType || types.empty() || types.size() != nums.size())
            return false;

        CellClass* const pTargetCell = MapClass::Instance.TryGetCellAt(target);
        if (!pTargetCell)
            return false;

        // ScenarioInit suppresses the side effects of object creation (sounds,
        // discovery, and in some builds free-unit style bonuses). Antares brackets
        // its creation the same way.
        ++Unsorted::ScenarioInit;
        auto const pPlane = static_cast<AircraftClass*>(pPlaneType->CreateObject(pOwner));
        --Unsorted::ScenarioInit;

        if (!pPlane)
            return false;

        pPlane->Spawned = true;

        const auto spawnCell = MapClass::Instance.PickCellOnEdge(
            entry, CellStruct::Empty, CellStruct::Empty,
            SpeedType::Winged, true, MovementZone::Normal);

        pPlane->QueueMission(Mission::ParadropApproach, false);
        pPlane->SetTarget(pTargetCell);

        ++Unsorted::ScenarioInit;
        const bool spawned = pPlane->Unlimbo(CellClass::Cell2Coord(spawnCell), DirType::North);
        --Unsorted::ScenarioInit;

        if (!spawned)
        {
            GameDelete(pPlane);
            return false;
        }

        for (std::size_t i = 0; i < types.size(); ++i)
        {
            TechnoTypeClass* const pType = types[i];
            if (!pType)
                continue;

            // Only infantry and vehicles can be paradropped — same restriction
            // Antares enforces; anything else would be created and then stranded.
            const auto abs = pType->WhatAmI();
            if (abs != AbstractType::UnitType && abs != AbstractType::InfantryType)
            {
                Debug::Log("[SuperWeaponExt] paradrop: %s is not infantry or a "
                           "vehicle; skipped\n", pType->ID);
                continue;
            }

            for (int k = 0; k < nums[i]; ++k)
            {
                if (auto const pNew = pType->CreateObject(pOwner))
                {
                    pNew->Limbo();
                    pPlane->Passengers.AddPassenger(static_cast<FootClass*>(pNew));
                }
            }
        }

        pPlane->HasPassengers = true;
        pPlane->NextMission();
        return true;
    }
}

bool SWTypeExt::RunOwnedParaDrop(SuperWeaponTypeClass* pType, HouseClass* pFirer,
                                 const CellStruct& cell)
{
    if (!pType || !pFirer)
        return false;

    auto const pExt = SWTypeExt::ExtMap.Find(pType);
    if (!pExt || !pExt->ParaDrop.Enabled)
        return false;

    const ParaDropConfig& cfg = pExt->ParaDrop;

    if (!cfg.Aircraft || cfg.Types.empty())
    {
        Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop=yes but no valid "
                   "SWExt.ParaDrop.Aircraft / .Types; nothing dropped\n", pType->ID);
        return false;   // let the normal path try, rather than silently eating the shot
    }

    const Edge entry = ResolveEntryEdge(cfg, pFirer, cell);

    // Explicit offsets win over a generated formation.
    std::vector<SWExt::Offset> offsets = cfg.Offsets;
    if (offsets.empty())
    {
        offsets = SWExt::BuildFormation(cfg.Kind, cfg.Planes, cfg.Spacing,
                                        FromEngineEdge(entry));
    }

    int launched = 0;
    int queued   = 0;

    for (std::size_t i = 0; i < offsets.size(); ++i)
    {
        CellStruct target = cell;
        target.X = static_cast<short>(target.X + offsets[i].X);
        target.Y = static_cast<short>(target.Y + offsets[i].Y);

        const int delay = i < cfg.Delays.size() ? cfg.Delays[i] : 0;

        if (delay > 0)
        {
            PendingDrop p;
            p.LaunchFrame = Unsorted::CurrentFrame + delay;
            p.Aircraft    = cfg.Aircraft;
            p.Owner       = pFirer;
            p.Target      = target;
            p.Entry       = entry;
            p.Types       = cfg.Types;
            p.Nums        = cfg.Nums;
            g_pending.push_back(std::move(p));
            ++queued;
            continue;
        }

        if (LaunchPlane(pFirer, cfg.Aircraft, target, entry, cfg.Types, cfg.Nums))
            ++launched;
    }

    Debug::Log("[SuperWeaponExt] [%s] paradrop at (%d,%d): edge %d, %d plane(s) "
               "away, %d queued\n", pType->ID, cell.X, cell.Y,
               static_cast<int>(entry), launched, queued);

    // Handled even if some planes failed to spawn — the alternative is letting
    // Antares fire a second, unconfigured drop on top of ours.
    return launched > 0 || queued > 0;
}

void SWTypeExt::TickPendingParaDrops()
{
    if (g_pending.empty())
        return;

    const int now = Unsorted::CurrentFrame;

    for (std::size_t i = 0; i < g_pending.size();)
    {
        if (g_pending[i].LaunchFrame > now)
        {
            ++i;
            continue;
        }

        const PendingDrop p = g_pending[i];
        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));

        // The owner may have been defeated between queueing and launching.
        if (p.Owner && !p.Owner->Defeated)
            LaunchPlane(p.Owner, p.Aircraft, p.Target, p.Entry, p.Types, p.Nums);
    }
}

// =============================================================================
// Per-frame tick — LogicClass::AI, immediately after the object-update loop
//
// 0x55B6B3 is the post-loop seat: every object has already ticked this frame.
// Only an unmerged Phobos PR touches it, so it is uncontended in any release
// build. Documented in the encyclopedia's Logic-Frame-Update page.
// =============================================================================
DEFINE_HOOK(0x55B6B3, LogicClass_AI_SWExtParaDropTick, 0x5)
{
    SWTypeExt::TickPendingParaDrops();
    return 0;
}

// =============================================================================
// "When does it start dropping?" — per-AircraftType ParadropRadius
//
// The engine decides to begin dropping by comparing the plane's distance to its
// target against RulesClass->ParadropRadius (+0x54C, defaulted to 0x400 = 4
// cells in the RulesClass constructor at 0x665D1D). That is a single [General]
// value governing EVERY paradrop in the game — the limitation this replaces.
//
//   415991:  mov  0x8871e0,%ecx           ; RulesClass::Instance
//   415997:  cmp  0x54c(%ecx),%eax        ; <-- our hook. EAX = distance
//   41599d:  jle  0x4159c8                ; close enough -> the DROP branch
//   41599f:  ...                          ; too far -> keep flying
//
// 0x4159C8 leads to `call 0x415C60` = AircraftClass::Paradrop, confirming which
// branch is which. The sibling path at 0x41593A does the same test.
//
// ⚠ Both handlers ALWAYS return an explicit address and never 0. The stolen
// bytes are only the `cmp`, so returning 0 would re-run it against the unchanged
// rules value and ignore our override entirely.
//
// Scope note: this is per AIRCRAFT TYPE, not per superweapon, because the hook
// only has the plane. Give two superweapons different plane types to give them
// different drop radii. Per-SW would need per-instance state keyed on the
// aircraft, which is not worth a lifetime-tracking map yet.
// =============================================================================
namespace
{
    // Resolve the drop radius for a plane, in leptons. <0 means "not overridden".
    int OverriddenParadropRadius(AircraftClass* pPlane)
    {
        if (!pPlane || !pPlane->Type)
            return -1;

        auto const pExt = TechnoTypeExt::ExtMap.Find(pPlane->Type);
        return pExt ? pExt->ParadropRadius : -1;
    }

    // Shared decision for both test sites.
    bool ShouldDropNow(AircraftClass* pPlane, int distance)
    {
        const int overridden = OverriddenParadropRadius(pPlane);
        const int radius = overridden >= 0
            ? overridden
            : RulesClass::Instance->ParadropRadius;

        return distance <= radius;
    }
}

DEFINE_HOOK(0x415997, AircraftClass_Mission_ParadropOverfly_Radius, 0x6)
{
    enum { Drop = 0x4159C8, KeepFlying = 0x41599F };

    GET(AircraftClass*, pThis, ESI);
    GET(int, distance, EAX);

    return ShouldDropNow(pThis, distance) ? Drop : KeepFlying;
}

DEFINE_HOOK(0x41593A, AircraftClass_Mission_ParadropApproach_Radius, 0x6)
{
    enum { Near = 0x415942, Far = 0x415956 };

    GET(AircraftClass*, pThis, ESI);
    GET(int, distance, EAX);

    return ShouldDropNow(pThis, distance) ? Near : Far;
}

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

#include <Ext/Techno/StandingOrders.h>

#include <Ext/TechnoType/Body.h>   // TechnoTypeExt — the ParadropRadius override

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <CellClass.h>
#include <DisplayClass.h>
#include <Fundamentals.h>   // Unsorted::CurrentFrame
#include <HouseClass.h>
#include <MapClass.h>
#include <RulesClass.h>   // RulesClass::Instance->ParadropRadius
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
        CellStruct         Spawn       {};
        Edge               Entry       = Edge::North;
        int                Radius      = -1;   // per-SW drop radius, leptons
        std::vector<TechnoTypeClass*> Types;
        std::vector<int>              Nums;
    };

    std::vector<PendingDrop> g_pending;

    // One-shot cursor re-arm for SWExt.KeepSelectedAfterFire. -1 = nothing
    // pending. Purely local UI state, so it is never serialized and never
    // consulted by game logic.
    int g_keepSelectedIndex = -1;

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
    // -------------------------------------------------------------------------
    // Per-superweapon drop radius: which SW launched this plane?
    //
    // The engine's drop test only has the AIRCRAFT, so a per-superweapon radius
    // needs the plane->SW association recorded at launch. We own the launch, so
    // we can record it; Antares-launched drops are simply absent from this table
    // and fall through to the per-aircraft-type key.
    //
    // ⚠ RAW POINTERS AS KEYS. This is the shape that caused an unbounded leak
    // and stale-pointer reuse in AggressiveStance, so three rules apply:
    //   1. the read path uses a linear FIND and never a default-inserting
    //      operator[] -- a lookup must never grow the table;
    //   2. entries are swept every frame from the tick we already own, so a dead
    //      plane cannot linger;
    //   3. the recorded TYPE is verified on read, so if an address is recycled
    //      into a different object the entry is ignored rather than believed.
    // Worst case after all three is a wrong drop distance for one frame, never a
    // crash. The table is also tiny -- one entry per plane in flight.
    // -------------------------------------------------------------------------
    struct PlaneRadius
    {
        AircraftClass*     Plane;
        AircraftTypeClass* Type;    // guards against address reuse
        int                Radius;  // leptons
        bool               Logged;  // diagnostic only, one line per plane
    };

    std::vector<PlaneRadius> g_planeRadius;

    void RememberPlaneRadius(AircraftClass* pPlane, int radius)
    {
        if (!pPlane || radius < 0)
            return;

        for (auto& e : g_planeRadius)
        {
            if (e.Plane == pPlane)
            {
                e.Type   = pPlane->Type;
                e.Radius = radius;
                return;
            }
        }

        g_planeRadius.push_back(PlaneRadius{ pPlane, pPlane->Type, radius, false });
    }

    // <0 = this plane has no per-SW radius.
    int RecalledPlaneRadius(AircraftClass* pPlane)
    {
        if (!pPlane)
            return -1;

        for (auto const& e : g_planeRadius)
        {
            if (e.Plane == pPlane && e.Type == pPlane->Type)
                return e.Radius;
        }
        return -1;
    }

    void SweepPlaneRadius()
    {
        for (std::size_t i = 0; i < g_planeRadius.size(); )
        {
            AircraftClass* const p = g_planeRadius[i].Plane;
            if (!p || !p->IsAlive || p->InLimbo)
            {
                g_planeRadius[i] = g_planeRadius.back();
                g_planeRadius.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    bool LaunchPlane(HouseClass* pOwner, AircraftTypeClass* pPlaneType,
                     const CellStruct& target, Edge entry,
                     const std::vector<TechnoTypeClass*>& types,
                     const std::vector<int>& nums,
                     int swRadius,
                     const CellStruct* pSpawnOverride = nullptr)
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

        // A caller building a formation supplies the entry cell so the planes
        // come in spread out; otherwise fall back to the engine's edge picker,
        // which is what Antares uses.
        const auto spawnCell = pSpawnOverride
            ? *pSpawnOverride
            : MapClass::Instance.PickCellOnEdge(
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

        // Only meaningful once the plane exists; skipped entirely when the
        // superweapon sets no radius of its own.
        RememberPlaneRadius(pPlane, swRadius);

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
                                        FromEngineEdge(entry), cfg.Align);
    }

    // Pick the entry cell ONCE for the whole drop, then spread the planes along
    // the edge by each one's sideways formation offset.
    //
    // Antares calls PickCellOnEdge per plane with identical arguments, which is
    // why stock multi-plane drops enter bunched at one point and only fan out at
    // the target. Sharing a base cell and offsetting from it means the formation
    // is visible on the way in, not just on arrival.
    //
    // Only the SIDEWAYS (perpendicular) component is applied. The forward
    // component would push planes off the edge line — further out is harmless in
    // principle but risks Unlimbo failing outside the map.
    const CellStruct baseSpawn = MapClass::Instance.PickCellOnEdge(
        entry, CellStruct::Empty, CellStruct::Empty,
        SpeedType::Winged, true, MovementZone::Normal);

    SWExt::Offset fwd{}, right{};
    SWExt::ApproachAxes(FromEngineEdge(entry), fwd, right);

    int launched = 0;
    int queued   = 0;

    for (std::size_t i = 0; i < offsets.size(); ++i)
    {
        CellStruct target = cell;
        target.X = static_cast<short>(target.X + offsets[i].X);
        target.Y = static_cast<short>(target.Y + offsets[i].Y);

        // Project this plane's offset onto the edge direction. `right` is a unit
        // vector, so the dot product is the sideways distance in cells.
        const int sideways = offsets[i].X * right.X + offsets[i].Y * right.Y;

        CellStruct spawn = baseSpawn;
        spawn.X = static_cast<short>(spawn.X + right.X * sideways);
        spawn.Y = static_cast<short>(spawn.Y + right.Y * sideways);

        const int delay = i < cfg.Delays.size() ? cfg.Delays[i] : 0;

        if (delay > 0)
        {
            PendingDrop p;
            p.LaunchFrame = Unsorted::CurrentFrame + delay;
            p.Aircraft    = cfg.Aircraft;
            p.Owner       = pFirer;
            p.Target      = target;
            p.Entry       = entry;
            p.Spawn       = spawn;
            p.Radius      = cfg.Radius;
            p.Types       = cfg.Types;
            p.Nums        = cfg.Nums;
            g_pending.push_back(std::move(p));
            ++queued;
            continue;
        }

        if (LaunchPlane(pFirer, cfg.Aircraft, target, entry, cfg.Types, cfg.Nums,
                        cfg.Radius, &spawn))
            ++launched;
    }

    Debug::Log("[SuperWeaponExt] [%s] paradrop at (%d,%d): edge %d, %d plane(s) "
               "away, %d queued, radius %d\n", pType->ID, cell.X, cell.Y,
               static_cast<int>(entry), launched, queued, cfg.Radius);

    // Handled even if some planes failed to spawn — the alternative is letting
    // Antares fire a second, unconfigured drop on top of ours.
    return launched > 0 || queued > 0;
}

void SWTypeExt::RequestKeepSelected(int swIndex)
{
    g_keepSelectedIndex = swIndex;
}

void SWTypeExt::TickPendingParaDrops()
{
    // Service a pending cursor re-arm first. The engine deselected during
    // SuperClass::Launch on the frame the superweapon fired; this puts it back.
    //
    // Consumed unconditionally, so it only ever fires once — otherwise a player
    // right-clicking to cancel would be overridden on the very next frame.
    if (g_keepSelectedIndex >= 0)
    {
        const int index = g_keepSelectedIndex;
        g_keepSelectedIndex = -1;

        auto const pPlayer = HouseClass::CurrentPlayer;
        if (pPlayer && DisplayClass::Instance.CurrentSWTypeIndex == -1)
        {
            // Only re-arm something the player still actually owns.
            if (pPlayer->Supers.GetItemOrDefault(index))
                DisplayClass::Instance.CurrentSWTypeIndex = index;
        }
    }

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
            LaunchPlane(p.Owner, p.Aircraft, p.Target, p.Entry, p.Types, p.Nums,
                        p.Radius, &p.Spawn);
    }
}

// =============================================================================
// Per-frame tick — LogicClass::AI, immediately after the object-update loop
//
// 0x55B6B3 is the post-loop seat: every object has already ticked this frame.
// Only an unmerged Phobos PR touches it, so it is uncontended in any release
// build. Documented in the encyclopedia's Logic-Frame-Update page.
// =============================================================================
// ⚠ EVERY per-frame consumer in this DLL shares THIS handler. Do not add a
// second DEFINE_HOOK nearby: a 5-byte hook here covers 0x55B6B3..0x55B6B7, so a
// hook at e.g. 0x55B6B6 would OVERLAP rather than chain. Syringe chains
// same-address hooks safely but overlapping ones corrupt each other's stubs into
// a wild jump. Call your tick from here instead.
DEFINE_HOOK(0x55B6B3, LogicClass_AI_SWExtFrameTick, 0x5)
{
    SWTypeExt::TickPendingParaDrops();
    SWExt::StandingOrders::Tick();
    SweepPlaneRadius();
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
// Scope: the hook only has the PLANE, so a per-superweapon radius needs the
// plane->SW association recorded at launch. It now is — see the plane registry
// above RememberPlaneRadius. Resolution order is SW, then aircraft type, then
// the [General] global, so the per-aircraft-type key still works unchanged and
// two superweapons sharing one plane type can differ.
//
// Only OWNED drops (SWExt.ParaDrop=yes) can carry a per-SW radius, because only
// those pass through our LaunchPlane. An Antares-run paradrop is absent from the
// registry and resolves at the aircraft-type level, which is the old behaviour.
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
    //
    // Precedence, most specific first:
    //   1. SWExt.ParaDrop.Radius on the SUPERWEAPON that launched this plane
    //   2. SWExt.ParadropRadius on the AIRCRAFT TYPE
    //   3. [General]ParadropRadius, the engine's single global
    bool ShouldDropNow(AircraftClass* pPlane, int distance)
    {
        const char* source = "SW";
        int radius = RecalledPlaneRadius(pPlane);

        if (radius < 0)
        {
            source = "aircraft type";
            radius = OverriddenParadropRadius(pPlane);
        }

        if (radius < 0)
        {
            source = "[General]";
            radius = RulesClass::Instance->ParadropRadius;
        }

        const bool drop = distance <= radius;

        // One line per plane, the first time it actually starts dropping. Says
        // WHICH level of the precedence chain won, because the configured value
        // alone cannot show that — the whole point of the per-SW key is that it
        // beats a different per-aircraft-type value, and only the resolved
        // number proves it did.
        if (drop)
        {
            for (auto& e : g_planeRadius)
            {
                if (e.Plane == pPlane && e.Type == pPlane->Type && !e.Logged)
                {
                    e.Logged = true;
                    Debug::Log("[SuperWeaponExt] plane starts dropping at %d "
                               "leptons (%d cells), radius from %s\n",
                               radius, radius / 256, source);
                    break;
                }
            }
        }

        return drop;
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

#pragma once
/*
 * SuperWeaponExt — player-placed beacons that weapons and superweapons react to.
 *
 * ENGINE-FREE, like AutoFire.h / Constraint.h / Money.h / Formation.h.
 *
 * ============================================================================
 * THE BEACON IS A TECHNO. That decision is the whole design.
 * ============================================================================
 *
 * The temptation is to invent a new object class holding a cell, a radius and
 * an owner. Don't. Spawn an invisible dummy TechnoType instead, because being
 * in TechnoClass::Array is what makes everything else work for free:
 *
 *   - Weapons can target it, because weapons target technos.
 *   - SWExt designators/inhibitors already see it: Ext/SWType/Body.cpp walks
 *     TechnoClass::Array (line ~676), so listing the beacon's type in
 *     SWExt.Designators= makes superweapons extend to it with NO NEW CODE.
 *     Same for SWExt.Inhibitors= if you want a beacon that suppresses instead.
 *   - Antares' own SW.Designators / SW.AITargetingType see it too.
 *   - Ownership, sight, death, removal and save/load all come from TechnoClass.
 *
 * What you must switch OFF deliberately, because a real techno has them:
 *   - shootability by anyone (Armor / Insignificant / Invisible),
 *   - cell occupancy, or beacons block pathing,
 *   - selection, score and unit counts.
 *
 * ============================================================================
 * TWO PRIMITIVES, KEPT SEPARATE
 * ============================================================================
 *
 * MARKER  — the techno above plus a range. Answers "what is here, whose is it,
 *           how far does it reach". Entirely passive.
 * TRIGGER — this file. Answers "what fires at it, how often, who reacts".
 *
 * Keeping them apart is what stops the INI collapsing into special cases: a
 * marker with no trigger is just a designator, and a trigger can point at any
 * marker. Fusing them would mean a new tag for every combination.
 *
 * ⚠ DETERMINISM, and the same trap AutoFire.h documents. A beacon's POSITION is
 * synced simulation state once placed, so auto-fire may aim at it — unlike the
 * mouse or the local view, which is why AutoFireTarget deliberately has no
 * Mouse or Screen mode. But PLACEMENT is a player action on one client, so it
 * must travel as a network event, exactly like vanilla beacon placement
 * (EventClass type 0x12, built at 0x4AC22C). Applying a placement locally
 * desyncs the moment another client's pathing or targeting sees a different
 * map. This is the mistake CommandBarExt's no-go zones currently still have.
 *
 * Firing, by contrast, is applied DIRECTLY on every client rather than queued —
 * see AutoFire.h's note: queueing would have each of N clients enqueue the same
 * launch and produce N launches.
 */
#include <vector>

#include "Constraint.h"   // Relation

namespace SWExt
{
    // How listed units react to a beacon of their house.
    enum class BeaconUnitResponse : unsigned char
    {
        None = 0,      // ignore it; the beacon is only for superweapons
        InRangeOnly,   // engage only what is already inside the beacon radius
        AttackMove,    // attack-move to the beacon, engaging on the way
        Aggressive,    // attack-move AND acquire targets they would normally skip
    };

    // A beacon TYPE, one INI section. Instances are spawned from it.
    struct BeaconRule
    {
        bool Enabled = false;

        // --- marker half -----------------------------------------------------
        int  TechnoTypeIndex = -1;  // the dummy techno spawned to represent it
        int  Range = 5;             // cells; the radius everything else measures
        int  Lifetime = -1;         // frames the beacon lives, <0 = until removed
        int  Cooldown = 0;          // frames before this house may place another
        int  MaxActive = 1;         // simultaneous beacons of this type per house

        // --- superweapon half ------------------------------------------------
        // Positionally matched: FireSWs[i] fires every FireIntervals[i] frames
        // while the beacon lives. An interval of 0 means "once, on placement".
        std::vector<int> FireSWs;
        std::vector<int> FireIntervals;

        // false = fire directly, ignoring charge and cameo (the beacon IS the
        // delivery mechanism). true = go through the sidebar, so the shot still
        // costs the player their charge and respects normal availability.
        bool UseSidebar = false;

        // --- unit half -------------------------------------------------------
        std::vector<int> UnitTypes;   // empty = every unit the house owns
        BeaconUnitResponse Response = BeaconUnitResponse::None;

        // Let responding units exceed their own weapon range to hit things in
        // the beacon. Off by default: this is a balance lever, not a default.
        bool BreakRangeLimits = false;

        // How far away a unit notices the beacon at all. <0 = the beacon Range.
        int UnitNoticeRange = -1;
    };

    // A live beacon on the map.
    struct BeaconInstance
    {
        int   RuleIndex = -1;
        int   HouseIndex = -1;
        short CellX = 0;
        short CellY = 0;
        int   ExpiryFrame = -1;   // <0 = never
        int   NextFireFrame = 0;  // per-instance so intervals do not drift
    };
}

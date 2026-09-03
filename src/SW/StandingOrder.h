#pragma once
/*
 * SuperWeaponExt — unit standing orders.
 *
 * ENGINE-FREE, like Constraint.h and Formation.h. The engine adapter reduces
 * live objects to plain data, this picks the destination, and the adapter issues
 * the move.
 *
 * WHAT THIS IS. A persistent per-TechnoType rule: "whenever you have nothing
 * better to do, head toward X." X is either the nearest instance of some
 * TechnoType, or the last cell a superweapon landed on for your house.
 *
 * ⚠ DETERMINISM. Unlike the cursor features, this ISSUES ORDERS — it changes
 * simulation state. So it must be computed from synced state only, on the same
 * frame on every client. That is exactly what the engine's own AI does, and it
 * is why nothing here may read the local player, the mouse, or the view. The
 * adapter keys the interval off Unsorted::CurrentFrame for the same reason.
 */
#include <cstdint>
#include <vector>

#include "Constraint.h"   // Relation, Matches — same vocabulary as inhibitors

namespace SWExt
{
    enum class StandingOrderMode : unsigned char
    {
        None = 0,   // no standing order (the default; costs nothing)
        Techno,     // move toward the nearest matching TechnoType
        SWImpact,   // move toward the last superweapon impact for the owner
    };

    // A candidate destination, reduced to plain data by the engine adapter.
    struct OrderTarget
    {
        int      TypeIndex = -1;
        Relation Rel       = Relation::None;   // relation to the ORDERED unit's owner
        int      CellX     = 0;
        int      CellY     = 0;
        bool     Active    = false;
    };

    struct StandingOrder
    {
        StandingOrderMode Mode = StandingOrderMode::None;

        std::vector<int> TypeIndices;                  // for Techno mode
        Relation         Affects  = Relation::Enemies; // whose technos to head for
        int              Range    = 0;                 // cells; 0 = whole map
        int              Interval = 45;                // frames between re-evaluations
        bool             IdleOnly = true;              // only redirect idle units

        bool Active() const
        {
            if (this->Mode == StandingOrderMode::SWImpact)
                return true;
            return this->Mode == StandingOrderMode::Techno && !this->TypeIndices.empty();
        }

        bool CoversType(int typeIndex) const
        {
            for (int idx : this->TypeIndices)
                if (idx == typeIndex)
                    return true;
            return false;
        }
    };

    // Squared cell distance, exact integer. Same discipline as Constraint.h:
    // this feeds a decision that changes simulation state, so no floats.
    inline std::int64_t DistanceSq(int ax, int ay, int bx, int by)
    {
        const std::int64_t dx = static_cast<std::int64_t>(ax) - bx;
        const std::int64_t dy = static_cast<std::int64_t>(ay) - by;
        return dx * dx + dy * dy;
    }

    // Pick the nearest eligible target for a unit standing at (unitX, unitY).
    // Returns nullptr when nothing qualifies, which the adapter treats as
    // "leave this unit alone" rather than "stop what you are doing".
    //
    // ⚠ Ties are broken by the EARLIEST index in `targets`, never by iteration
    // order over a hash or by distance alone. The array order is itself synced,
    // so every client picks the same one — a tie broken differently on two
    // clients would send the same unit to two different places.
    inline const OrderTarget* PickNearest(const StandingOrder& order,
                                          const std::vector<OrderTarget>& targets,
                                          int unitX, int unitY)
    {
        if (!order.Active())
            return nullptr;

        const std::int64_t rangeSq = static_cast<std::int64_t>(order.Range) * order.Range;

        const OrderTarget* best = nullptr;
        std::int64_t bestDist = 0;

        for (const auto& t : targets)
        {
            if (!t.Active)
                continue;
            if (!Matches(order.Affects, t.Rel))
                continue;
            if (!order.CoversType(t.TypeIndex))
                continue;

            const std::int64_t d = DistanceSq(unitX, unitY, t.CellX, t.CellY);
            if (order.Range > 0 && d > rangeSq)
                continue;

            // Strictly-less keeps the first of equals, which is what makes the
            // tie-break deterministic.
            if (!best || d < bestDist)
            {
                best = &t;
                bestDist = d;
            }
        }

        return best;
    }

    // Should this unit re-evaluate on this frame?
    //
    // Staggered by the unit's array index so a thousand units do not all
    // recompute on the same tick. The stagger is a pure function of two synced
    // values, so every client staggers identically.
    inline bool DueThisFrame(const StandingOrder& order, int frame, int unitIndex)
    {
        if (!order.Active())
            return false;

        const int interval = order.Interval > 0 ? order.Interval : 1;
        // Unit index may be large; keep the modulo positive.
        const int phase = ((unitIndex % interval) + interval) % interval;
        return ((frame % interval) == phase);
    }
}

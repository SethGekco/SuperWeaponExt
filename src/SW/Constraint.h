#pragma once
/*
 * SuperWeaponExt — the constraint core.
 *
 * DELIBERATELY ENGINE-FREE. Nothing in this header includes YRpp or touches a
 * game type. That is what lets tests/constraint_test.cpp build and run on Linux
 * in CI, where the Windows DLL cannot. The engine adapter that fills in Source
 * lives in src/Ext/SWType/Hooks.Launch.cpp.
 *
 * MODEL (see FINDINGS.md §0): inhibitors and designators are *restrictions*, and
 * restrictions compose as AND regardless of evaluation order. We therefore never
 * replace Antares' verdict — we add ours on top as a veto. A modder who wants our
 * semantics leaves Antares' SW.Inhibitors/SW.Designators empty, which makes its
 * check a no-op and ours the only one running.
 *
 * INTEGER MATH IS NOT AN ACCIDENT. Every distance comparison here is exact
 * integer arithmetic. Antares uses DistanceFromSquared, which returns double;
 * for a value that gates a network-synced launch decision, float rounding that
 * differs between clients is a desync. Do not "simplify" this to doubles.
 */
#include <cstdint>
#include <vector>

namespace SWExt
{
    // Relation of some techno's owner TO the house firing the superweapon.
    // Bit values deliberately match Antares' SuperWeaponAffectedHouse
    // (src/Utilities/Enums.h:68) so INI values read the same way modders already
    // expect from SW.AffectsHouse.
    enum class Relation : unsigned char
    {
        None    = 0x0,
        Owner   = 0x1,
        Allies  = 0x2,
        Enemies = 0x4,

        Team      = Owner | Allies,
        NotAllies = Owner | Enemies,
        NotOwner  = Allies | Enemies,
        All       = Owner | Allies | Enemies,
    };

    inline Relation operator|(Relation a, Relation b)
    {
        return static_cast<Relation>(static_cast<unsigned char>(a) | static_cast<unsigned char>(b));
    }

    inline bool Matches(Relation mask, Relation value)
    {
        return (static_cast<unsigned char>(mask) & static_cast<unsigned char>(value)) != 0;
    }

    enum class Rank : unsigned char { Rookie = 0, Veteran = 1, Elite = 2 };

    // Per-TechnoType range, veterancy-tiered. A tier of <0 means "inherit the
    // next tier down"; base <0 means "the caller should substitute Sight".
    struct RangeSpec
    {
        int Base    = -1;
        int Veteran = -1;
        int Elite   = -1;

        // Resolve for a rank, falling back down the tiers. `sight` is the
        // engine's default when nothing at all was configured.
        int Resolve(Rank rank, int sight) const
        {
            int value = -1;
            if (rank == Rank::Elite)                  value = this->Elite;
            if (value < 0 && rank >= Rank::Veteran)   value = this->Veteran;
            if (value < 0)                            value = this->Base;
            return value < 0 ? sight : value;
        }
    };

    // One candidate techno, already reduced to plain data by the engine adapter.
    struct Source
    {
        int      TypeIndex   = -1;
        Relation Rel         = Relation::None;   // owner's relation to the firer
        Rank     Vet         = Rank::Rookie;
        int      CellX       = 0;
        int      CellY       = 0;
        bool     Active      = false;   // alive && health && !inLimbo && !deactivated
        bool     Powered     = true;    // buildings: IsPowerOnline(); others: true
        int      FallbackRange = 0;     // TechnoType range, already veterancy-resolved
    };

    // One role's rule (inhibitor or designator) as parsed from a [SOMESW] section.
    struct Rule
    {
        std::vector<int> TypeIndices;      // TechnoType array indices
        std::vector<int> RangesByIndex;    // parallel to TypeIndices; <0 = use fallback
        bool             Any          = false;
        Relation         Affects      = Relation::None;
        bool             RequirePower = true;

        // An empty type list with Any=false means the modder did not configure
        // this role at all, so it must not constrain anything.
        bool Active() const { return this->Any || !this->TypeIndices.empty(); }

        // Per-SW range override for a type, or <0 to fall back to the TechnoType's.
        // This is the "index" from the wishlist: it is what lets one building
        // inhibit two different superweapons at two different radii.
        int OverrideRangeFor(int typeIndex) const
        {
            for (std::size_t i = 0; i < this->TypeIndices.size(); ++i)
            {
                if (this->TypeIndices[i] == typeIndex)
                    return i < this->RangesByIndex.size() ? this->RangesByIndex[i] : -1;
            }
            return -1;
        }

        bool CoversType(int typeIndex) const
        {
            if (this->Any)
                return true;
            for (int idx : this->TypeIndices)
                if (idx == typeIndex)
                    return true;
            return false;
        }
    };

    // Does `src` count for this rule, and is the target cell inside its radius?
    inline bool IsEligible(const Rule& rule, const Source& src, int cellX, int cellY)
    {
        if (!src.Active)
            return false;
        if (rule.RequirePower && !src.Powered)
            return false;
        if (!Matches(rule.Affects, src.Rel))
            return false;
        if (!rule.CoversType(src.TypeIndex))
            return false;

        const int override_ = rule.OverrideRangeFor(src.TypeIndex);
        const int range = override_ >= 0 ? override_ : src.FallbackRange;
        if (range <= 0)
            return false;

        // Exact integer distance. See the header note: no doubles on this path.
        const std::int64_t dx = static_cast<std::int64_t>(src.CellX) - cellX;
        const std::int64_t dy = static_cast<std::int64_t>(src.CellY) - cellY;
        const std::int64_t r  = static_cast<std::int64_t>(range);
        return dx * dx + dy * dy <= r * r;
    }

    // The whole verdict for one (superweapon, target cell) pair.
    //
    // Semantics deliberately mirror Antares so that a mod migrating from
    // SW.Inhibitors to SWExt.Inhibitors sees no behavioural surprise:
    //   - designators: rule inactive => pass; otherwise need AT LEAST ONE in range
    //   - inhibitors:  rule inactive => pass; otherwise fail if ANY is in range
    inline bool Allows(const Rule& inhibitors, const Rule& designators,
                       const std::vector<Source>& sources, int cellX, int cellY)
    {
        if (designators.Active())
        {
            bool found = false;
            for (const auto& src : sources)
            {
                if (IsEligible(designators, src, cellX, cellY)) { found = true; break; }
            }
            if (!found)
                return false;
        }

        if (inhibitors.Active())
        {
            for (const auto& src : sources)
            {
                if (IsEligible(inhibitors, src, cellX, cellY))
                    return false;
            }
        }

        return true;
    }

    // TODO(§2b): range growth/shrink over time, and ratio-vs-proximity scaling.
    // Both belong here, as a transform applied to `range` inside IsEligible.
    // Growth MUST be driven by the synced frame counter (Unsorted::CurrentFrame)
    // or a saved per-techno counter — never GetTickCount, never render state.
    // Proximity ratio needs a per-frame cache first: IsEligible already runs
    // once per techno per cursor frame, and a nested scan makes that O(n^2).
}

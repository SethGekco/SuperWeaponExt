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

    // YR logic runs at 15 frames/second, so a minute is 900 frames. Antares uses
    // the same `* 900.0` conversion for every minute-valued rules key.
    inline constexpr int FramesPerMinute = 900;

    // Floor division. Integer `/` truncates toward zero, which would make a
    // shrinking range contract slower than a growing one expands at the same
    // magnitude. Flooring keeps growth and shrink symmetric, and is exact
    // integer arithmetic so it stays lockstep-safe.
    inline std::int64_t FloorDiv(std::int64_t n, std::int64_t d)
    {
        const std::int64_t q = n / d;
        return (n % d != 0 && ((n < 0) != (d < 0))) ? q - 1 : q;
    }

    // Range that changes as the match progresses.
    //
    // ⚠ Driven by the SYNCED frame counter (Unsorted::CurrentFrame), never by
    // wall-clock or render time. Every client must compute the same radius on
    // the same frame or the launch verdict diverges.
    // Growth rates are stored as THOUSANDTHS of a cell per minute, so a modder
    // can write `Growth=0.25` and get a quarter-cell per minute. An integer
    // cells-per-minute rate would have floored the slowest useful setting at
    // 1 cell/min, which is far too coarse for a long match.
    //
    // The INI value is parsed to a double ONCE, at rules-load time, and
    // immediately converted to this integer. Everything downstream is exact
    // integer arithmetic, so no float ever reaches a per-frame calculation
    // where clients could round apart.
    inline constexpr int GrowthScale = 1000;

    struct GrowthSpec
    {
        int MilliPerMinute = 0;   // thousandths of a cell per minute; negative shrinks
        int Min            = -1;  // clamp floor, <0 = none
        int Max            = -1;  // clamp ceiling, <0 = none

        bool Active() const { return this->MilliPerMinute != 0; }

        int DeltaAt(int frames) const
        {
            if (!this->Active() || frames <= 0)
                return 0;

            return static_cast<int>(FloorDiv(
                static_cast<std::int64_t>(this->MilliPerMinute) * frames,
                static_cast<std::int64_t>(FramesPerMinute) * GrowthScale));
        }
    };

    // Range that scales with how many of certain technos exist, or are near the
    // inhibitor/designator itself.
    struct RatioSpec
    {
        std::vector<int> TypeIndices;
        Relation Affects = Relation::All;
        int Range   = 0;   // cells around the SOURCE to count within; 0 = whole map
        int PerUnit = 0;   // cells added per counted techno; negative shrinks
        int Max     = 0;   // cap on the total bonus's magnitude; 0 = uncapped

        bool Active() const
        {
            return this->PerUnit != 0 && !this->TypeIndices.empty();
        }

        bool CountsType(int typeIndex) const
        {
            for (int idx : this->TypeIndices)
                if (idx == typeIndex)
                    return true;
            return false;
        }
    };

    // A techno that may be COUNTED by a RatioSpec. Separate from Source because
    // the sets rarely overlap: one is "things that inhibit", the other is
    // "things that make an inhibitor stronger".
    struct RatioSource
    {
        int      TypeIndex = -1;
        Relation Rel       = Relation::None;
        int      CellX     = 0;
        int      CellY     = 0;
        bool     Active    = false;
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

        // Range modifiers, applied to whatever base range resolved. Both live on
        // the RULE (per superweapon) rather than the TechnoType, so "this
        // building inhibits SW A on a growing radius and SW B on a fixed one" is
        // expressible — and so there is no ambiguity about whether they apply on
        // top of a per-SW Ranges override. They always do.
        GrowthSpec Growth;
        RatioSpec  Ratio;

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

    // Everything time- or world-dependent that the evaluator needs, gathered
    // ONCE per evaluation by the engine adapter.
    //
    // ⚠ Ratio sources are collected in a single pass over the techno array and
    // then reused for every candidate. Counting them per-candidate instead would
    // make the cursor path O(n²) — and this whole evaluation already runs once
    // per cursor frame.
    struct EvalContext
    {
        int Frames = 0;   // Unsorted::CurrentFrame — SYNCED. Never wall-clock.
        std::vector<RatioSource> RatioSources;
    };

    // How many ratio-countable technos this rule sees from `src`'s position.
    inline int CountRatioFor(const Rule& rule, const Source& src,
                             const EvalContext& ctx)
    {
        if (!rule.Ratio.Active())
            return 0;

        const std::int64_t r = rule.Ratio.Range;
        int count = 0;

        for (const auto& cand : ctx.RatioSources)
        {
            if (!cand.Active)
                continue;
            if (!Matches(rule.Ratio.Affects, cand.Rel))
                continue;
            if (!rule.Ratio.CountsType(cand.TypeIndex))
                continue;

            // Range 0 means "anywhere on the map" — a pure existence count.
            if (r > 0)
            {
                const std::int64_t dx = static_cast<std::int64_t>(cand.CellX) - src.CellX;
                const std::int64_t dy = static_cast<std::int64_t>(cand.CellY) - src.CellY;
                if (dx * dx + dy * dy > r * r)
                    continue;
            }

            ++count;
        }

        return count;
    }

    // The full range pipeline for one source under one rule:
    //   base (per-SW override, else the veterancy-resolved TechnoType range)
    //   + growth over match time
    //   + ratio bonus
    //   clamped
    // Returns <= 0 when the source should not constrain anything.
    inline int EffectiveRange(const Rule& rule, const Source& src,
                              const EvalContext& ctx)
    {
        const int override_ = rule.OverrideRangeFor(src.TypeIndex);
        std::int64_t range = override_ >= 0 ? override_ : src.FallbackRange;

        range += rule.Growth.DeltaAt(ctx.Frames);

        if (rule.Ratio.Active())
        {
            std::int64_t bonus =
                static_cast<std::int64_t>(rule.Ratio.PerUnit) * CountRatioFor(rule, src, ctx);

            // Max caps the bonus's magnitude, so it works for shrink too.
            if (rule.Ratio.Max > 0)
            {
                const std::int64_t cap = rule.Ratio.Max;
                if (bonus >  cap) bonus =  cap;
                if (bonus < -cap) bonus = -cap;
            }

            range += bonus;
        }

        // Growth clamps bound the FINAL range, not just the growth term — that
        // is what makes "starts at 4, creeps to 20, never further" expressible.
        if (rule.Growth.Min >= 0 && range < rule.Growth.Min)
            range = rule.Growth.Min;
        if (rule.Growth.Max >= 0 && range > rule.Growth.Max)
            range = rule.Growth.Max;

        return static_cast<int>(range);
    }

    // Does `src` count for this rule, and is the target cell inside its radius?
    inline bool IsEligible(const Rule& rule, const Source& src, int cellX, int cellY,
                           const EvalContext& ctx = {})
    {
        if (!src.Active)
            return false;
        if (rule.RequirePower && !src.Powered)
            return false;
        if (!Matches(rule.Affects, src.Rel))
            return false;
        if (!rule.CoversType(src.TypeIndex))
            return false;

        const int range = EffectiveRange(rule, src, ctx);
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
                       const std::vector<Source>& sources, int cellX, int cellY,
                       const EvalContext& ctx = {})
    {
        if (designators.Active())
        {
            bool found = false;
            for (const auto& src : sources)
            {
                if (IsEligible(designators, src, cellX, cellY, ctx)) { found = true; break; }
            }
            if (!found)
                return false;
        }

        if (inhibitors.Active())
        {
            for (const auto& src : sources)
            {
                if (IsEligible(inhibitors, src, cellX, cellY, ctx))
                    return false;
            }
        }

        return true;
    }
}

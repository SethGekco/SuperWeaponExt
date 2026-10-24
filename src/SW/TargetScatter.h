#pragma once
/*
 * SuperWeaponExt — scatter a superweapon's target by a few cells.
 *
 * ENGINE-FREE, like Constraint.h / Money.h / Formation.h / StandingOrder.h /
 * AutoFire.h. The random draws and the constraint evaluation both happen in the
 * adapter, behind callbacks, so everything that decides an outcome is testable
 * off-target.
 *
 * ============================================================================
 * WHAT THIS COMPLEMENTS
 * ============================================================================
 *
 * Antares' `SW.AITargeting=` is EXACT: `Base` means the base centre, every
 * launch, forever. For a superweapon that destroys something that hardly
 * matters; for one that DELIVERS something — a building, a paradrop — it means
 * every launch of the match lands on the same tile.
 *
 *     AITargeting.RandomX=-4,4          ; AI-owned shots
 *     AITargeting.RandomY=-4,4
 *     AITargeting.RandomX.Human=0,0     ; ...and human-owned shots separately
 *     AITargeting.RandomY.Human=0,0
 *
 * `.Human` is a SUFFIX RULE, not a one-off: append it to any key here and the
 * value applies to shots fired by a human-controlled house, defaulting to the
 * AI value when absent. Which side a launch uses is decided by the OWNING
 * HOUSE (`IsControlledByHuman`), which is synced state and admits no ambiguity.
 *
 * ============================================================================
 * TWO STAGES, AND WHY THE CHECKS ARE PER AXIS
 * ============================================================================
 *
 * A scattered shot has two interesting cells: the one targeting chose, and the
 * one the dice moved it to. Inhibitors and designators can be enforced at
 * either, or both — and, at the random stage, PER AXIS:
 *
 *     AITargeting.RandomX.TargetRequireDesignator=yes   ; the PRE-random cell
 *     AITargeting.RandomX.TargetAvoidInhibitors=yes
 *     AITargeting.RandomX.RandomRequireDesignator=yes   ; the POST-random cell
 *     AITargeting.RandomX.RandomAvoidInhibitors=yes
 *     ...and the same four on RandomY, and a `.Human` twin for each.
 *
 * ⚠ A PER-AXIS CHECK IS NOT A CHECK ON HALF A CELL. "Is a designator in range"
 * has no X-only answer. What an axis's flag actually says is **whether that
 * axis's DISPLACEMENT has to stay legal**, and it is answered by probing the
 * aim point displaced on THAT AXIS ALONE:
 *
 *     both axes enabled ->  probe (x + rollX, y + rollY)   the whole move
 *     X only            ->  probe (x + rollX, y       )    X must stay legal
 *     Y only            ->  probe (x       , y + rollY)    Y must stay legal
 *     neither           ->  no probe
 *
 * That is what makes the split worth having. `RandomY=-5,5` honouring
 * designators while `RandomX=-25,25` ignores them means: the shot may wander
 * twenty-five cells sideways, but its vertical displacement must keep it on
 * covered ground. One flag cannot express that; two can.
 *
 * Where both are enabled the probe is the whole displacement, which is strictly
 * the harshest of the three — so "the reading that forbids wins" holds, and the
 * all-yes default is exactly a plain check of the cell the shot lands on.
 *
 * The TARGET stage has no displacement, so all three enabled cases collapse to
 * the aim point and the pair behaves as a simple OR. PlanProbe below produces
 * that for free, which is why one function serves both stages.
 *
 * Designators and inhibitors are planned SEPARATELY: their axis flags are
 * independent, so a launch can end up probing designators at (x, y+rollY) and
 * inhibitors at (x+rollX, y+rollY) in the same evaluation.
 *
 * Failing the TARGET stage refuses the launch: the shot was aimed somewhere it
 * may not go. Failing the RANDOM stage re-rolls — see ResolveScatter — and only
 * refuses when no roll works and the unscattered cell will not do either.
 *
 * ============================================================================
 * ⚠ DETERMINISM — read before touching the adapter
 * ============================================================================
 *
 * Draws MUST come from the in-match synced generator
 * (`ScenarioClass::Instance->Random`), never `Randomizer::Global`, whose own
 * YRpp header says it is for the map generator and other randomness OUTSIDE a
 * match.
 *
 * ResolveScatter draws BOTH offsets, in X-then-Y order, on every attempt —
 * never skipping one because its band is degenerate. The NUMBER of attempts
 * varies with the outcome, and that is fine: every client evaluates the same
 * launch on the same frame from the same synced inputs, so every client takes
 * the same number of attempts and consumes the same number of values. What
 * would desync is a client skipping a draw another client made, which is
 * exactly what a "don't bother rolling Y, its band is 0,0" optimisation would
 * cause. Do not add one.
 */

namespace SWExt
{
    // Where the scatter is allowed to put a cell. Mirrors the engine's
    // MapClass::MapCoordBounds, which is an LTRB of cell coordinates.
    struct ScatterBounds
    {
        int Left   = 0;
        int Top    = 0;
        int Right  = 0;
        int Bottom = 0;

        bool Usable() const
        {
            return this->Right >= this->Left && this->Bottom >= this->Top;
        }
    };

    // `min,max` per axis, in cells.
    struct ScatterBand
    {
        int MinX = 0;
        int MaxX = 0;
        int MinY = 0;
        int MaxY = 0;

        // Written back by the parser when it had to swap a reversed band, so
        // the adapter can say so once instead of silently fixing it.
        bool SwappedX = false;
        bool SwappedY = false;

        bool Active() const
        {
            return this->MinX != 0 || this->MaxX != 0
                || this->MinY != 0 || this->MaxY != 0;
        }

        // A reversed band (`4,-4`) is a typo, not a request for an empty range.
        // Normalising rather than rejecting keeps a mod working; the parser
        // records that it happened so the log can mention it.
        void Normalise()
        {
            if (this->MinX > this->MaxX)
            {
                const int t = this->MinX;
                this->MinX = this->MaxX;
                this->MaxX = t;
                this->SwappedX = true;
            }

            if (this->MinY > this->MaxY)
            {
                const int t = this->MinY;
                this->MinY = this->MaxY;
                this->MaxY = t;
                this->SwappedY = true;
            }
        }
    };

    // Which constraints this ONE AXIS insists on keeping legal.
    struct ScatterAxisChecks
    {
        bool TargetRequireDesignator = true;
        bool TargetAvoidInhibitors   = true;
        bool RandomRequireDesignator = true;
        bool RandomAvoidInhibitors   = true;
    };

    struct ScatterChecks
    {
        ScatterAxisChecks X;
        ScatterAxisChecks Y;

        bool AnyTarget() const
        {
            return this->X.TargetRequireDesignator || this->Y.TargetRequireDesignator
                || this->X.TargetAvoidInhibitors   || this->Y.TargetAvoidInhibitors;
        }

        bool AnyRandom() const
        {
            return this->X.RandomRequireDesignator || this->Y.RandomRequireDesignator
                || this->X.RandomAvoidInhibitors   || this->Y.RandomAvoidInhibitors;
        }
    };

    // Where one per-axis-gated check should be evaluated.
    //
    // `(x,y)` is the aim point and `(cx,cy)` the candidate. See the header
    // comment: an axis flag means "this axis's displacement must stay legal",
    // so an enabled axis contributes its own coordinate to the probe and a
    // disabled one contributes the aim point's.
    //
    // With `(cx,cy) == (x,y)` — the target stage, where nothing has moved —
    // every enabled combination yields the aim point, so the two flags behave
    // as a plain OR without that needing its own code path.
    struct ProbePoint
    {
        int  X = 0;
        int  Y = 0;
        bool Enabled = false;
    };

    inline ProbePoint PlanProbe(bool enableX, bool enableY,
                                int x, int y, int cx, int cy)
    {
        if (enableX && enableY)
            return ProbePoint{ cx, cy, true };
        if (enableX)
            return ProbePoint{ cx, y, true };
        if (enableY)
            return ProbePoint{ x, cy, true };

        return ProbePoint{ x, y, false };
    }

    struct ScatterSide
    {
        ScatterBand   Band;
        ScatterChecks Checks;
    };

    // One superweapon's complete scatter configuration.
    struct ScatterConfig
    {
        ScatterSide AI;
        ScatterSide Human;

        // The early-out for the launch hook: a superweapon with no band on
        // either side is not ours to touch and costs nothing.
        bool Active() const
        {
            return this->AI.Band.Active() || this->Human.Band.Active();
        }

        const ScatterSide& For(bool firerIsHuman) const
        {
            return firerIsHuman ? this->Human : this->AI;
        }
    };

    struct ScatterResult
    {
        int  X = 0;
        int  Y = 0;
        bool Shifted = false;   // the cell actually moved
        bool Clamped = false;   // the map edge cut the shift short
    };

    // Apply two already-drawn offsets to a cell.
    //
    // `rollX` / `rollY` are what the caller got back from the generator over
    // [MinX,MaxX] and [MinY,MaxY]. They are clamped to their own band here as
    // well, so a caller that passes something out of range cannot push a cell
    // somewhere the configuration never allowed.
    //
    // Out-of-bounds results are CLAMPED to the map rather than rejected: a
    // superweapon aimed near the edge should still scatter, just not off the
    // map.
    inline ScatterResult ApplyScatter(const ScatterBand& band, int x, int y,
                                      int rollX, int rollY,
                                      const ScatterBounds& bounds)
    {
        ScatterResult out;
        out.X = x;
        out.Y = y;

        if (!band.Active())
            return out;

        if (rollX < band.MinX) rollX = band.MinX;
        if (rollX > band.MaxX) rollX = band.MaxX;
        if (rollY < band.MinY) rollY = band.MinY;
        if (rollY > band.MaxY) rollY = band.MaxY;

        int nx = x + rollX;
        int ny = y + rollY;

        if (bounds.Usable())
        {
            const int cx = nx < bounds.Left ? bounds.Left
                         : (nx > bounds.Right ? bounds.Right : nx);
            const int cy = ny < bounds.Top ? bounds.Top
                         : (ny > bounds.Bottom ? bounds.Bottom : ny);

            out.Clamped = (cx != nx) || (cy != ny);
            nx = cx;
            ny = cy;
        }

        out.X = nx;
        out.Y = ny;
        out.Shifted = (nx != x) || (ny != y);
        return out;
    }

    // How many rolls a shot gets before giving up on finding a legal cell.
    // Bounded on purpose: this runs inside a launch, and an unbounded search
    // over a band that happens to contain no legal cell would hang the frame.
    inline constexpr int ScatterAttempts = 8;

    struct ScatterOutcome
    {
        int  X = 0;
        int  Y = 0;
        bool Fire    = true;    // false = refuse the launch entirely
        bool Shifted = false;
        bool Clamped = false;
        bool UsedOriginal = false;  // every roll failed; fell back to the aim point
        int  Attempts = 0;
    };

    // THE decision for the random stage.
    //
    // `roll(min, max)` draws one offset. `accept(x, y)` answers whether that
    // cell passes whatever Random-stage constraints are enabled — the adapter
    // supplies the real inhibitor/designator evaluation; the tests supply a
    // fake, which is the point of taking it as a parameter.
    //
    // "Move it somewhere else that does, or just not fire":
    //   1. up to `attempts` rolls; the first acceptable one wins
    //   2. otherwise the UNSCATTERED cell, if that is itself acceptable
    //   3. otherwise refuse
    //
    // ⚠ Both offsets are drawn on EVERY attempt, in X-then-Y order, even when
    // a band is degenerate. See the determinism note at the top of this file.
    template <typename RollFn, typename AcceptFn>
    inline ScatterOutcome ResolveScatter(const ScatterBand& band, int x, int y,
                                         const ScatterBounds& bounds,
                                         RollFn&& roll, AcceptFn&& accept,
                                         int attempts = ScatterAttempts)
    {
        ScatterOutcome out;
        out.X = x;
        out.Y = y;

        if (!band.Active())
            return out;

        for (int i = 0; i < attempts; ++i)
        {
            const int rollX = roll(band.MinX, band.MaxX);
            const int rollY = roll(band.MinY, band.MaxY);

            ++out.Attempts;

            auto const r = ApplyScatter(band, x, y, rollX, rollY, bounds);
            if (!accept(r.X, r.Y))
                continue;

            out.X       = r.X;
            out.Y       = r.Y;
            out.Shifted = r.Shifted;
            out.Clamped = r.Clamped;
            return out;
        }

        // No roll produced a cell the Random-stage checks accept. The aim point
        // is the only other candidate the configuration sanctions, and it is
        // held to the same checks — otherwise `RandomRequireDesignator=yes`
        // would be quietly bypassed by every exhausted search.
        out.UsedOriginal = true;
        out.Fire = accept(x, y);
        return out;
    }
}

#pragma once
/*
 * SuperWeaponExt — scatter an AI-chosen superweapon target by a few cells.
 *
 * ENGINE-FREE, like Constraint.h / Money.h / Formation.h / StandingOrder.h /
 * AutoFire.h. The random draws happen in the adapter; everything decidable
 * without them lives here and is tested off-target.
 *
 * ============================================================================
 * WHAT THIS COMPLEMENTS
 * ============================================================================
 *
 * Antares' `SW.AITargeting=` picks a cell for a superweapon the AI fires — and
 * for one the player fires, when `SW.UseAITargeting=yes`. Every mode of it is
 * EXACT: `Base` means the base centre, every time, forever. A superweapon that
 * delivers something to a cell therefore delivers it to the same cell on every
 * launch, which reads as mechanical and makes the result trivially predictable.
 *
 * These rules add the missing jitter:
 *
 *     SWExt.AITargeting.RandomX=-4,4     ; shift up to 4 cells either way
 *     SWExt.AITargeting.RandomY=-4,4
 *
 * Both are `min,max` in CELLS, inclusive, signed, defaulting to `0,0` — so an
 * unset rule is exactly the old behaviour and costs nothing. The range need not
 * straddle zero: `2,6` always shifts right by two to six cells.
 *
 * ============================================================================
 * ⚠ DETERMINISM — read before touching the adapter
 * ============================================================================
 *
 * The draws MUST come from the in-match synced generator
 * (`ScenarioClass::Instance->Random`), never `Randomizer::Global`, whose own
 * header says it is for the map generator and other out-of-match randomness.
 *
 * And the adapter must draw BOTH values, in X-then-Y order, on every launch
 * where the rule is active — never skipping one because its band happens to be
 * degenerate. Every client evaluates the same launch on the same frame, so an
 * identical sequence of draws leaves every client's generator in the same
 * state. A client that draws once where another draws twice desyncs the whole
 * match from that point on, which is a far worse failure than a wrong cell.
 *
 * That is why ApplyScatter takes the rolls as PARAMETERS. The core cannot skip
 * a draw it never makes, and the test suite can pin exact outcomes.
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

    struct ScatterRule
    {
        // `min,max` per axis, in cells. 0,0 = no shift on that axis.
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

    struct ScatterResult
    {
        int  X = 0;
        int  Y = 0;
        bool Shifted = false;   // the cell actually moved
        bool Clamped = false;   // the map edge cut the shift short
    };

    // Apply two already-drawn offsets to a cell.
    //
    // `rollX` / `rollY` are what the adapter got back from RandomRanged over
    // [MinX,MaxX] and [MinY,MaxY]. They are clamped to their own band here as
    // well, so a caller that passes something out of range cannot push a cell
    // somewhere the configuration never allowed.
    //
    // Out-of-bounds results are CLAMPED to the map rather than rejected: a
    // superweapon aimed near the edge should still scatter, just not off the
    // map. The caller decides what to do when even the clamped cell is illegal
    // — the adapter falls back to the unshifted cell, which is always legal
    // because the targeting that produced it said so.
    inline ScatterResult ApplyScatter(const ScatterRule& rule, int x, int y,
                                      int rollX, int rollY,
                                      const ScatterBounds& bounds)
    {
        ScatterResult out;
        out.X = x;
        out.Y = y;

        if (!rule.Active())
            return out;

        if (rollX < rule.MinX) rollX = rule.MinX;
        if (rollX > rule.MaxX) rollX = rule.MaxX;
        if (rollY < rule.MinY) rollY = rule.MinY;
        if (rollY > rule.MaxY) rollY = rule.MaxY;

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
}

#pragma once
/*
 * SuperWeaponExt — paradrop formation geometry.
 *
 * ENGINE-FREE, like Constraint.h, so the pattern maths is unit-tested on Linux
 * rather than eyeballed in-game. The engine adapter turns these offsets into
 * actual drop cells.
 *
 * Patterns are laid out in APPROACH SPACE, not map space: "forward" is the
 * direction the planes are flying, "right" is perpendicular to it. That is what
 * makes a `line` read as planes abreast and a `column` as planes nose-to-tail
 * regardless of which map edge they entered from. Vanilla's paradrop has no
 * concept of this at all — every plane picks its own random edge cell.
 */
#include <cstdint>
#include <vector>

namespace SWExt
{
    // Matches YRpp's Edge enum: None=-1, North=0, East=1, South=2, West=3.
    enum class ApproachEdge : int
    {
        None  = -1,
        North = 0,
        East  = 1,
        South = 2,
        West  = 3,
    };

    // Which space the pattern's SIDEWAYS axis is laid out in.
    //
    // ⚠ This exists because RA2's isometric projection is not conformal: a right
    // angle in cell space is NOT a right angle on screen. Projecting the two cell
    // axes gives
    //     cell +X -> screen (+2, +1)
    //     cell +Y -> screen (-2, +1)
    // whose dot product is -3, i.e. ~127 degrees apart, not 90. So a line built
    // perpendicular to the flight path IN CELL SPACE looks swept back from some
    // edges and swept forward from others. Reported in play as "a row in some
    // directions, trailing behind each other in others" — the rotation was
    // working, just in the space the player does not see.
    enum class FormationAlign : unsigned char
    {
        Screen = 0,  // perpendicular ON SCREEN — looks abreast from every edge
        Cell,        // perpendicular in cell space (mathematically tidy, looks skewed)
        Map,         // never rotates; the spread is always along cell +X
    };

    enum class Formation : unsigned char
    {
        Line = 0,   // abreast, perpendicular to the flight path
        Column,     // nose-to-tail along the flight path
        Wedge,      // a V, apex leading
        Box,        // rectangular grid
    };

    struct Offset
    {
        int X = 0;
        int Y = 0;

        friend bool operator==(const Offset& a, const Offset& b)
        {
            return a.X == b.X && a.Y == b.Y;
        }
    };

    // Unit vectors for "the way the planes are flying" and "to their right",
    // given the edge they entered from. A plane entering at the NORTH edge flies
    // south, so forward is +Y.
    inline void ApproachAxes(ApproachEdge edge, Offset& forward, Offset& right)
    {
        switch (edge)
        {
        case ApproachEdge::East:  forward = { -1,  0 }; right = {  0, -1 }; break;
        case ApproachEdge::South: forward = {  0, -1 }; right = { -1,  0 }; break;
        case ApproachEdge::West:  forward = {  1,  0 }; right = {  0,  1 }; break;
        case ApproachEdge::North:
        default:                  forward = {  0,  1 }; right = {  1,  0 }; break;
        }
    }

    // The cell-space direction that appears PERPENDICULAR TO THE FLIGHT PATH on
    // screen, as an integer numerator over ScreenRightDen.
    //
    // Derived by projecting the flight vector to screen with the engine's own
    // transform, rotating 90 degrees there, and converting back through the
    // inverse of the 2x2 isometric matrix. For a 2:1 projection that yields
    // (1.25, 0.75) for a north approach and (0.75, 1.25) for an east one —
    // scaled by 4 below to stay in integers.
    inline constexpr int ScreenRightDen = 4;

    inline Offset ScreenRightNumerator(ApproachEdge edge)
    {
        switch (edge)
        {
        case ApproachEdge::East:  return {  3,  5 };
        case ApproachEdge::West:  return { -3, -5 };
        case ApproachEdge::South: return { -5, -3 };
        case ApproachEdge::North:
        default:                  return {  5,  3 };
        }
    }

    // Per-plane target offsets relative to the superweapon's aim cell.
    //
    // `spacing` is in cells. A count of 1 always yields a single {0,0} so a
    // one-plane drop lands exactly where the player clicked.
    inline std::vector<Offset> BuildFormation(Formation kind, int count,
                                              int spacing, ApproachEdge edge,
                                              FormationAlign align = FormationAlign::Screen)
    {
        std::vector<Offset> out;
        if (count <= 0)
            return out;

        out.reserve(static_cast<std::size_t>(count));

        if (count == 1 || spacing == 0)
        {
            out.assign(static_cast<std::size_t>(count), Offset{ 0, 0 });
            return out;
        }

        Offset fwd{}, rgt{};
        ApproachAxes(edge, fwd, rgt);

        // `forward` is always the true flight vector — a column trails along the
        // actual path, which already looks right on screen. Only the SIDEWAYS
        // axis needs correcting, because that is the one the eye compares
        // against the flight direction.
        Offset rightNum = rgt;
        int    rightDen = 1;

        if (align == FormationAlign::Screen)
        {
            rightNum = ScreenRightNumerator(edge);
            rightDen = ScreenRightDen;
        }
        else if (align == FormationAlign::Map)
        {
            fwd      = Offset{ 0, 1 };   // fixed axes, never rotates
            rightNum = Offset{ 1, 0 };
            rightDen = 1;
        }

        auto place = [&](int alongForward, int alongRight)
        {
            out.push_back(Offset{
                fwd.X * alongForward + (rightNum.X * alongRight) / rightDen,
                fwd.Y * alongForward + (rightNum.Y * alongRight) / rightDen });
        };

        switch (kind)
        {
        case Formation::Column:
            // Nose-to-tail. Index 0 leads, so later planes arrive later — the
            // cheapest way to get time separation without any pending state.
            for (int i = 0; i < count; ++i)
                place(-i * spacing, 0);
            break;

        case Formation::Wedge:
        {
            // Apex first, then alternating out to either side and trailing back.
            place(0, 0);
            int rank = 1;
            for (int i = 1; i < count; ++i)
            {
                const int side = (i % 2) ? 1 : -1;
                place(-rank * spacing, side * rank * spacing);
                if (i % 2 == 0)
                    ++rank;
            }
            break;
        }

        case Formation::Box:
        {
            // Squarish grid, centred. Rows trail backwards.
            int cols = 1;
            while (cols * cols < count)
                ++cols;

            const int rows = (count + cols - 1) / cols;
            for (int i = 0; i < count; ++i)
            {
                const int r = i / cols;
                const int c = i % cols;
                // Centre each axis: (2*index - (n-1)) / 2 without fractions by
                // working in half-spacing units, then halving once.
                const int rightHalf   = (2 * c - (cols - 1)) * spacing;
                const int forwardHalf = (2 * r - (rows - 1)) * spacing;
                place(-forwardHalf / 2, rightHalf / 2);
            }
            break;
        }

        case Formation::Line:
        default:
        {
            // Abreast and centred on the aim cell.
            for (int i = 0; i < count; ++i)
            {
                const int rightHalf = (2 * i - (count - 1)) * spacing;
                place(0, rightHalf / 2);
            }
            break;
        }
        }

        return out;
    }

    // Which map edge is closest to `cell`, for Origin=nearest. Bounds are the
    // playable area's left/top/right/bottom in cells.
    inline ApproachEdge NearestEdge(int cellX, int cellY,
                                    int left, int top, int right, int bottom)
    {
        const int dLeft   = cellX - left;
        const int dRight  = right - cellX;
        const int dTop    = cellY - top;
        const int dBottom = bottom - cellY;

        int best = dTop;
        ApproachEdge edge = ApproachEdge::North;

        if (dRight  < best) { best = dRight;  edge = ApproachEdge::East;  }
        if (dBottom < best) { best = dBottom; edge = ApproachEdge::South; }
        if (dLeft   < best) { best = dLeft;   edge = ApproachEdge::West;  }

        return edge;
    }
}

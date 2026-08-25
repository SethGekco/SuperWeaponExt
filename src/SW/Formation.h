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

    // Per-plane target offsets relative to the superweapon's aim cell.
    //
    // `spacing` is in cells. A count of 1 always yields a single {0,0} so a
    // one-plane drop lands exactly where the player clicked.
    inline std::vector<Offset> BuildFormation(Formation kind, int count,
                                              int spacing, ApproachEdge edge)
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

        auto place = [&](int alongForward, int alongRight)
        {
            out.push_back(Offset{
                fwd.X * alongForward + rgt.X * alongRight,
                fwd.Y * alongForward + rgt.Y * alongRight });
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

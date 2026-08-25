/*
 * Off-target tests for the paradrop formation geometry.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/formation_test.cpp -o ft && ./ft
 */
#include <SW/Formation.h>

#include <cstdio>

using namespace SWExt;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(expr, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL  %s\n        at %s:%d\n", (what), __FILE__, __LINE__); \
        }                                                                        \
    } while (0)

namespace
{
    bool Contains(const std::vector<Offset>& v, int x, int y)
    {
        for (auto const& o : v)
            if (o.X == x && o.Y == y)
                return true;
        return false;
    }

    // Every offset distinct? Two planes sharing a drop cell is a bug.
    bool AllDistinct(const std::vector<Offset>& v)
    {
        for (std::size_t i = 0; i < v.size(); ++i)
            for (std::size_t j = i + 1; j < v.size(); ++j)
                if (v[i] == v[j])
                    return false;
        return true;
    }
}

// -----------------------------------------------------------------------------
static void Test_Degenerate()
{
    std::printf("degenerate counts\n");

    CHECK(BuildFormation(Formation::Line, 0, 4, ApproachEdge::North).empty(),
          "zero planes yields no offsets");
    CHECK(BuildFormation(Formation::Line, -3, 4, ApproachEdge::North).empty(),
          "negative count yields no offsets");

    auto one = BuildFormation(Formation::Line, 1, 4, ApproachEdge::North);
    CHECK(one.size() == 1 && one[0] == (Offset{ 0, 0 }),
          "a single plane lands exactly on the aim cell");

    // Spacing 0 = everyone on the same cell; degenerate but must not crash or
    // silently produce a different count.
    auto stacked = BuildFormation(Formation::Line, 4, 0, ApproachEdge::North);
    CHECK(stacked.size() == 4, "spacing 0 still yields one offset per plane");
    CHECK(Contains(stacked, 0, 0), "spacing 0 stacks them on the aim cell");
}

// -----------------------------------------------------------------------------
static void Test_CountAlwaysMatches()
{
    std::printf("count integrity across all patterns\n");

    const Formation kinds[] = { Formation::Line, Formation::Column,
                                Formation::Wedge, Formation::Box };
    const ApproachEdge edges[] = { ApproachEdge::North, ApproachEdge::East,
                                   ApproachEdge::South, ApproachEdge::West };

    bool ok = true;
    for (auto k : kinds)
        for (auto e : edges)
            for (int n = 1; n <= 12; ++n)
                if (static_cast<int>(BuildFormation(k, n, 3, e).size()) != n)
                    ok = false;

    CHECK(ok, "every pattern/edge/count combination yields exactly `count` offsets");
}

// -----------------------------------------------------------------------------
static void Test_LineIsPerpendicular()
{
    std::printf("line: abreast, perpendicular to flight\n");

    // Entering from the north, planes fly SOUTH (+Y), so a line spreads on X.
    auto north = BuildFormation(Formation::Line, 3, 4, ApproachEdge::North);
    CHECK(Contains(north, -4, 0) && Contains(north, 0, 0) && Contains(north, 4, 0),
          "north approach spreads the line along X");

    // Entering from the west, planes fly EAST (+X), so the line spreads on Y.
    auto west = BuildFormation(Formation::Line, 3, 4, ApproachEdge::West);
    CHECK(Contains(west, 0, -4) && Contains(west, 0, 0) && Contains(west, 0, 4),
          "west approach spreads the line along Y instead");

    CHECK(AllDistinct(north) && AllDistinct(west), "no two planes share a cell");
}

// -----------------------------------------------------------------------------
static void Test_ColumnTrailsBackwards()
{
    std::printf("column: nose-to-tail along the flight path\n");

    // North approach flies +Y, so trailing planes sit at NEGATIVE Y — behind the
    // leader, which is what makes them arrive later.
    auto col = BuildFormation(Formation::Column, 3, 5, ApproachEdge::North);
    CHECK(col[0] == (Offset{ 0, 0 }),   "the leader is on the aim cell");
    CHECK(col[1] == (Offset{ 0, -5 }),  "the second trails behind by one spacing");
    CHECK(col[2] == (Offset{ 0, -10 }), "the third trails by two");

    // Same shape, rotated: east approach flies -X, so trailing is +X.
    auto east = BuildFormation(Formation::Column, 3, 5, ApproachEdge::East);
    CHECK(east[1] == (Offset{ 5, 0 }), "trailing follows the approach axis, not the map");

    CHECK(AllDistinct(col), "column offsets are distinct");
}

// -----------------------------------------------------------------------------
static void Test_WedgeAndBox()
{
    std::printf("wedge + box\n");

    auto wedge = BuildFormation(Formation::Wedge, 5, 3, ApproachEdge::North);
    CHECK(wedge[0] == (Offset{ 0, 0 }), "wedge apex leads on the aim cell");
    CHECK(AllDistinct(wedge), "wedge offsets are distinct");

    bool bothSides = false;
    for (auto const& o : wedge)
        for (auto const& p : wedge)
            if (o.X > 0 && p.X < 0)
                bothSides = true;
    CHECK(bothSides, "wedge spreads to both sides");

    auto box = BuildFormation(Formation::Box, 9, 2, ApproachEdge::North);
    CHECK(box.size() == 9,        "3x3 box for 9 planes");
    CHECK(AllDistinct(box),       "box offsets are distinct");
    CHECK(Contains(box, 0, 0),    "an odd square box has a centre plane");

    // A non-square count must not drop or duplicate planes.
    auto box7 = BuildFormation(Formation::Box, 7, 2, ApproachEdge::North);
    CHECK(box7.size() == 7 && AllDistinct(box7),
          "a partial last row still yields distinct offsets");
}

// -----------------------------------------------------------------------------
static void Test_ApproachAxes()
{
    std::printf("approach axes\n");

    Offset f{}, r{};
    ApproachAxes(ApproachEdge::North, f, r);
    CHECK(f == (Offset{ 0, 1 }),  "entering from the north means flying south");
    ApproachAxes(ApproachEdge::South, f, r);
    CHECK(f == (Offset{ 0, -1 }), "entering from the south means flying north");
    ApproachAxes(ApproachEdge::East, f, r);
    CHECK(f == (Offset{ -1, 0 }), "entering from the east means flying west");
    ApproachAxes(ApproachEdge::West, f, r);
    CHECK(f == (Offset{ 1, 0 }),  "entering from the west means flying east");

    // forward and right must be perpendicular for every edge, or patterns skew.
    bool perp = true;
    for (auto e : { ApproachEdge::North, ApproachEdge::East,
                    ApproachEdge::South, ApproachEdge::West })
    {
        ApproachAxes(e, f, r);
        if (f.X * r.X + f.Y * r.Y != 0)
            perp = false;
    }
    CHECK(perp, "forward and right are perpendicular on every edge");
}

// -----------------------------------------------------------------------------
static void Test_NearestEdge()
{
    std::printf("nearest edge selection\n");

    // A 100x100 playable area.
    const int L = 0, T = 0, R = 100, B = 100;

    CHECK(NearestEdge(50,  5, L, T, R, B) == ApproachEdge::North, "near the top -> north");
    CHECK(NearestEdge(50, 95, L, T, R, B) == ApproachEdge::South, "near the bottom -> south");
    CHECK(NearestEdge(95, 50, L, T, R, B) == ApproachEdge::East,  "near the right -> east");
    CHECK(NearestEdge( 5, 50, L, T, R, B) == ApproachEdge::West,  "near the left -> west");

    // A corner resolves to one specific edge rather than flapping.
    const auto corner = NearestEdge(2, 2, L, T, R, B);
    CHECK(corner == NearestEdge(2, 2, L, T, R, B), "corner choice is stable");
    CHECK(corner == ApproachEdge::North || corner == ApproachEdge::West,
          "a top-left corner picks one of its two touching edges");

    // Dead centre must still answer deterministically.
    CHECK(NearestEdge(50, 50, L, T, R, B) == NearestEdge(50, 50, L, T, R, B),
          "the exact centre is deterministic");
}

int main()
{
    std::printf("SuperWeaponExt paradrop formation\n\n");

    Test_Degenerate();
    Test_CountAlwaysMatches();
    Test_LineIsPerpendicular();
    Test_ColumnTrailsBackwards();
    Test_WedgeAndBox();
    Test_ApproachAxes();
    Test_NearestEdge();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

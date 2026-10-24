/*
 * Off-target tests for AI-targeting scatter.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/targetscatter_test.cpp -o ts && ./ts
 */
#include <SW/TargetScatter.h>

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
    // A map big enough that nothing clamps unless a test means it to.
    ScatterBounds Wide()
    {
        return ScatterBounds{ 0, 0, 200, 200 };
    }

    ScatterRule Band(int minX, int maxX, int minY, int maxY)
    {
        ScatterRule r;
        r.MinX = minX; r.MaxX = maxX;
        r.MinY = minY; r.MaxY = maxY;
        r.Normalise();
        return r;
    }
}

// -----------------------------------------------------------------------------
static void Test_InertByDefault()
{
    std::printf("an unset rule changes nothing\n");

    ScatterRule none;
    CHECK(!none.Active(), "0,0 on both axes is inactive");

    // Even handed a non-zero roll — which cannot happen in the adapter, but is
    // the behaviour that makes the default provably free.
    auto const r = ApplyScatter(none, 50, 60, 7, -7, Wide());
    CHECK(r.X == 50 && r.Y == 60, "inactive rule leaves the cell alone");
    CHECK(!r.Shifted, "and reports no shift");
    CHECK(!r.Clamped, "and no clamp");
}

// -----------------------------------------------------------------------------
static void Test_ActiveDetection()
{
    std::printf("a rule is active if any band is non-zero\n");

    CHECK(Band(-1, 1, 0, 0).Active(), "X band alone activates");
    CHECK(Band(0, 0, -1, 1).Active(), "Y band alone activates");
    CHECK(Band(3, 3, 0, 0).Active(),  "a fixed non-zero offset activates");
    CHECK(!Band(0, 0, 0, 0).Active(), "all zero stays inactive");
}

// -----------------------------------------------------------------------------
static void Test_Offsets()
{
    std::printf("offsets land where the roll says\n");

    auto const rule = Band(-4, 4, -4, 4);

    auto const a = ApplyScatter(rule, 50, 60, 0, 0, Wide());
    CHECK(a.X == 50 && a.Y == 60, "a zero roll is a no-op");
    CHECK(!a.Shifted, "and is reported as unshifted");

    auto const b = ApplyScatter(rule, 50, 60, 3, -2, Wide());
    CHECK(b.X == 53 && b.Y == 58, "the roll is added per axis");
    CHECK(b.Shifted, "and is reported as shifted");

    auto const c = ApplyScatter(rule, 50, 60, -4, 4, Wide());
    CHECK(c.X == 46 && c.Y == 64, "the extremes of the band are reachable");
}

// -----------------------------------------------------------------------------
static void Test_AxesAreIndependent()
{
    std::printf("the axes do not leak into each other\n");

    auto const xOnly = Band(-5, 5, 0, 0);
    auto const r = ApplyScatter(xOnly, 50, 60, 5, 5, Wide());
    CHECK(r.X == 55, "X shifts");
    CHECK(r.Y == 60, "Y does not, because its band is 0,0 and the roll is clamped to it");
}

// -----------------------------------------------------------------------------
static void Test_OneSidedBands()
{
    std::printf("a band need not straddle zero\n");

    auto const right = Band(2, 6, 0, 0);
    auto const r = ApplyScatter(right, 50, 60, 2, 0, Wide());
    CHECK(r.X == 52, "the minimum of a one-sided band still shifts");
    CHECK(r.Shifted, "which counts as a shift");

    auto const far = ApplyScatter(right, 50, 60, 6, 0, Wide());
    CHECK(far.X == 56, "and so does its maximum");
}

// -----------------------------------------------------------------------------
static void Test_RollIsClampedToTheBand()
{
    std::printf("a roll outside the band cannot escape it\n");

    auto const rule = Band(-2, 2, -2, 2);

    auto const hi = ApplyScatter(rule, 50, 60, 99, 99, Wide());
    CHECK(hi.X == 52 && hi.Y == 62, "an over-large roll is cut to the maximum");

    auto const lo = ApplyScatter(rule, 50, 60, -99, -99, Wide());
    CHECK(lo.X == 48 && lo.Y == 58, "an under-small roll is cut to the minimum");
}

// -----------------------------------------------------------------------------
static void Test_ReversedBandIsNormalised()
{
    std::printf("a reversed band is a typo, not an empty range\n");

    ScatterRule r;
    r.MinX = 4;  r.MaxX = -4;
    r.MinY = -1; r.MaxY = 1;
    r.Normalise();

    CHECK(r.MinX == -4 && r.MaxX == 4, "the X band is swapped into order");
    CHECK(r.SwappedX, "and says so, so the adapter can warn once");
    CHECK(!r.SwappedY, "an ordered band is left alone");
    CHECK(r.MinY == -1 && r.MaxY == 1, "and keeps its values");
}

// -----------------------------------------------------------------------------
static void Test_ClampedToMap()
{
    std::printf("the map edge cuts a shift short rather than voiding it\n");

    auto const rule = Band(-10, 10, -10, 10);
    const ScatterBounds small{ 5, 5, 60, 60 };

    auto const low = ApplyScatter(rule, 7, 7, -10, -10, small);
    CHECK(low.X == 5 && low.Y == 5, "clamped to the top-left bound");
    CHECK(low.Clamped, "and reported as clamped");
    CHECK(low.Shifted, "a clamped shift is still a shift");

    auto const high = ApplyScatter(rule, 58, 58, 10, 10, small);
    CHECK(high.X == 60 && high.Y == 60, "clamped to the bottom-right bound");
    CHECK(high.Clamped, "and reported as clamped");

    auto const inside = ApplyScatter(rule, 30, 30, 4, -4, small);
    CHECK(inside.X == 34 && inside.Y == 26, "a shift well inside is untouched");
    CHECK(!inside.Clamped, "and is not reported as clamped");
}

// -----------------------------------------------------------------------------
static void Test_ClampCanCancelAShift()
{
    std::printf("clamping back onto the original cell is not a shift\n");

    auto const rule = Band(-10, 0, 0, 0);
    const ScatterBounds edge{ 5, 0, 60, 60 };

    auto const r = ApplyScatter(rule, 5, 20, -10, 0, edge);
    CHECK(r.X == 5 && r.Y == 20, "a cell already on the bound cannot move further out");
    CHECK(r.Clamped, "the clamp happened");
    CHECK(!r.Shifted, "but the cell did not move, so it is not a shift");
}

// -----------------------------------------------------------------------------
static void Test_UnusableBoundsAreIgnored()
{
    std::printf("nonsense bounds are ignored rather than trusted\n");

    auto const rule = Band(-3, 3, -3, 3);
    const ScatterBounds broken{ 100, 100, 0, 0 };

    CHECK(!broken.Usable(), "an inverted rect is not usable");

    auto const r = ApplyScatter(rule, 50, 60, 3, 3, broken);
    CHECK(r.X == 53 && r.Y == 63, "the shift still happens");
    CHECK(!r.Clamped, "and nothing claims to have clamped it");
}

// -----------------------------------------------------------------------------
int main()
{
    std::printf("SuperWeaponExt AI-targeting scatter\n\n");

    Test_InertByDefault();
    Test_ActiveDetection();
    Test_Offsets();
    Test_AxesAreIndependent();
    Test_OneSidedBands();
    Test_RollIsClampedToTheBand();
    Test_ReversedBandIsNormalised();
    Test_ClampedToMap();
    Test_ClampCanCancelAShift();
    Test_UnusableBoundsAreIgnored();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

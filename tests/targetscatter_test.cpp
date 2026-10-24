/*
 * Off-target tests for AI-targeting scatter.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/targetscatter_test.cpp -o ts && ./ts
 */
#include <SW/TargetScatter.h>

#include <cstdio>
#include <cstddef>
#include <functional>
#include <vector>

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

    ScatterBand Band(int minX, int maxX, int minY, int maxY)
    {
        ScatterBand r;
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

    ScatterBand none;
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

    ScatterBand r;
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
namespace
{
    // A scripted generator: hands back the next value in a list, so a test can
    // pin exactly which rolls happen and in what order. Counts its draws,
    // because "how many values did we consume" is the determinism property
    // that matters most here.
    struct FakeRoller
    {
        std::vector<int> Values;
        std::size_t Next     = 0;
        int         Draws    = 0;
        int         Fallback = 0;   // returned once Values runs out

        int operator()(int lo, int hi)
        {
            ++this->Draws;
            const int v = this->Next < this->Values.size()
                        ? this->Values[this->Next++] : this->Fallback;
            return v < lo ? lo : (v > hi ? hi : v);
        }
    };

    // Accepts every cell.
    auto AcceptAll = [](int, int) { return true; };

    // Accepts nothing.
    auto AcceptNone = [](int, int) { return false; };
}

// -----------------------------------------------------------------------------
static void Test_SidesAndDefaults()
{
    std::printf("the AI and Human sides are independent, checks default strict\n");

    ScatterConfig cfg;
    CHECK(!cfg.Active(), "a config with no band on either side is inactive");

    cfg.AI.Band = Band(-2, 2, -2, 2);
    CHECK(cfg.Active(), "an AI band alone activates the config");
    CHECK(cfg.For(false).Band.Active(), "the AI side carries it");
    CHECK(!cfg.For(true).Band.Active(), "and the human side is untouched");

    ScatterConfig human;
    human.Human.Band = Band(-1, 1, 0, 0);
    CHECK(human.Active(), "a human band alone also activates the config");
    CHECK(!human.For(false).Band.Active(), "without touching the AI side");

    const ScatterChecks def;
    CHECK(def.X.TargetRequireDesignator && def.Y.TargetRequireDesignator,
          "designators are required at the aim point by default, on both axes");
    CHECK(def.X.RandomAvoidInhibitors && def.Y.RandomAvoidInhibitors,
          "and inhibitors avoided at the scattered cell");
    CHECK(def.AnyTarget() && def.AnyRandom(), "so both stages are live by default");

    ScatterChecks off;
    off.X.TargetRequireDesignator = false;
    off.Y.TargetRequireDesignator = false;
    off.X.TargetAvoidInhibitors   = false;
    off.Y.TargetAvoidInhibitors   = false;
    CHECK(!off.AnyTarget(), "turning the target checks off on both axes skips the stage");
    CHECK(off.AnyRandom(),  "without disturbing the random stage");

    ScatterChecks half;
    half.X.TargetRequireDesignator = false;
    CHECK(half.AnyTarget(), "one axis still wanting a check keeps the stage alive");
}

// -----------------------------------------------------------------------------
static void Test_ProbePlanning()
{
    std::printf("an axis flag means 'this axis's displacement must stay legal'\n");

    // Aim point (50,60), candidate (75,65): +25 on X, +5 on Y.
    const int ax = 50, ay = 60, cx = 75, cy = 65;

    auto const both = PlanProbe(true, true, ax, ay, cx, cy);
    CHECK(both.Enabled, "both axes enabled is a live probe");
    CHECK(both.X == 75 && both.Y == 65, "...of the whole displacement");

    auto const xOnly = PlanProbe(true, false, ax, ay, cx, cy);
    CHECK(xOnly.Enabled, "X alone is a live probe");
    CHECK(xOnly.X == 75 && xOnly.Y == 60, "...of the X displacement only");

    auto const yOnly = PlanProbe(false, true, ax, ay, cx, cy);
    CHECK(yOnly.Enabled, "Y alone is a live probe");
    CHECK(yOnly.X == 50 && yOnly.Y == 65, "...of the Y displacement only");

    auto const none = PlanProbe(false, false, ax, ay, cx, cy);
    CHECK(!none.Enabled, "neither axis means no probe at all");
}

// -----------------------------------------------------------------------------
static void Test_ProbePlanningCollapsesAtTheTargetStage()
{
    std::printf("with no displacement the two flags behave as a plain OR\n");

    // The target stage passes the aim point as the candidate: nothing moved.
    const int ax = 50, ay = 60;

    for (int i = 0; i < 3; ++i)
    {
        const bool ex = (i != 2);
        const bool ey = (i != 1);

        auto const probe = PlanProbe(ex, ey, ax, ay, ax, ay);
        CHECK(probe.Enabled, "any enabled axis gives a live probe");
        CHECK(probe.X == 50 && probe.Y == 60, "...always at the aim point");
    }

    CHECK(!PlanProbe(false, false, ax, ay, ax, ay).Enabled,
          "and neither axis still means no probe");
}

// -----------------------------------------------------------------------------
static void Test_RexsCase_WideXFreeNarrowYConstrained()
{
    std::printf("the case this split exists for: X wanders, Y stays covered\n");

    // RandomX=-25,25 ignoring constraints; RandomY=-5,5 honouring them.
    // A designator covers a horizontal strip: any cell with |y - 60| <= 3.
    auto const band = Band(-25, 25, -5, 5);
    auto const covered = [](int, int y) { return y >= 57 && y <= 63; };

    // Roll far out on X and just inside on Y.
    {
        auto const probe = PlanProbe(/*X*/ false, /*Y*/ true, 50, 60, 75, 62);
        CHECK(probe.X == 50 && probe.Y == 62, "only the Y displacement is probed");
        CHECK(covered(probe.X, probe.Y), "which is inside the strip, so it passes");
    }

    // ...and the resolver accepts the far-out X because X is not probed.
    FakeRoller roll{ { 25, 2 }, 0, 0, 0 };
    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll),
        [&](int candX, int candY)
        {
            auto const probe = PlanProbe(false, true, 50, 60, candX, candY);
            return !probe.Enabled || covered(probe.X, probe.Y);
        });

    CHECK(r.Fire, "it fires");
    CHECK(r.X == 75, "25 cells sideways, unconstrained");
    CHECK(r.Y == 62, "and 2 cells down, which stayed on covered ground");
    CHECK(r.Attempts == 1, "accepted first try");

    // The same roll with BOTH axes enabled probes the real landing cell, which
    // is outside the strip on X... except the strip ignores X, so to show the
    // difference use a designator that covers a BOX around the aim point.
    auto const box = [](int x, int y)
    {
        return x >= 45 && x <= 55 && y >= 57 && y <= 63;
    };

    FakeRoller roll2{ {}, 0, 0, 0 };   // every roll is 0,0 -> the aim point
    auto const strict = ResolveScatter(band, 50, 60, Wide(), std::ref(roll2),
        [&](int candX, int candY)
        {
            auto const probe = PlanProbe(true, true, 50, 60, candX, candY);
            return !probe.Enabled || box(probe.X, probe.Y);
        });

    CHECK(strict.Fire, "with both axes enabled a zero roll still passes");
    CHECK(strict.X == 50 && strict.Y == 60, "landing on the aim point");

    FakeRoller roll3{ { 25, 0 }, 0, 0, 25 };   // always far out on X
    auto const refused = ResolveScatter(band, 50, 60, Wide(), std::ref(roll3),
        [&](int candX, int candY)
        {
            auto const probe = PlanProbe(true, true, 50, 60, candX, candY);
            return !probe.Enabled || box(probe.X, probe.Y);
        });

    CHECK(refused.UsedOriginal, "with both enabled, a far-out X is rejected every try");
    CHECK(refused.Fire, "and the aim point, which is inside the box, is used");
    CHECK(refused.X == 50 && refused.Y == 60, "so the shot lands where it was aimed");
}

// -----------------------------------------------------------------------------
static void Test_DesignatorAndInhibitorPlanIndependently()
{
    std::printf("the two constraint kinds plan their probes separately\n");

    // Designators: Y only. Inhibitors: both axes.
    auto const desig = PlanProbe(false, true, 50, 60, 75, 65);
    auto const inhib = PlanProbe(true,  true, 50, 60, 75, 65);

    CHECK(desig.X == 50 && desig.Y == 65, "the designator probe follows Y alone");
    CHECK(inhib.X == 75 && inhib.Y == 65, "the inhibitor probe follows both");
    CHECK(desig.X != inhib.X, "so one evaluation can probe two different cells");
}

// -----------------------------------------------------------------------------
static void Test_ResolveTakesTheFirstAcceptableRoll()
{
    std::printf("the first acceptable roll wins\n");

    auto const band = Band(-5, 5, -5, 5);
    FakeRoller roll{ { 3, -1 }, 0, 0, 0 };

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll), AcceptAll);
    CHECK(r.Fire, "it fires");
    CHECK(r.X == 53 && r.Y == 59, "at the first rolled cell");
    CHECK(r.Attempts == 1, "after one attempt");
    CHECK(roll.Draws == 2, "having drawn exactly two values");
    CHECK(!r.UsedOriginal, "and did not fall back");
}

// -----------------------------------------------------------------------------
static void Test_ResolveRerollsPastRefusedCells()
{
    std::printf("a refused cell is re-rolled, not accepted\n");

    auto const band = Band(-5, 5, -5, 5);
    // First two rolls land on cells the checks refuse; the third is fine.
    FakeRoller roll{ { 1, 0, 2, 0, 4, 0 }, 0, 0, 0 };

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll),
                                  [](int x, int) { return x >= 54; });

    CHECK(r.Fire, "it fires");
    CHECK(r.X == 54, "at the first cell that passes");
    CHECK(r.Attempts == 3, "after three attempts");
    CHECK(roll.Draws == 6, "two draws per attempt, always both axes");
}

// -----------------------------------------------------------------------------
static void Test_BothAxesDrawnEvenWhenDegenerate()
{
    std::printf("a 0,0 axis is still drawn -- the determinism rule\n");

    // Y band is 0,0. The Y draw must still happen, or clients whose config
    // differs would consume different numbers of values.
    auto const band = Band(-3, 3, 0, 0);
    FakeRoller roll{ { 2, 99 }, 0, 0, 0 };

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll), AcceptAll);
    CHECK(roll.Draws == 2, "both axes drew, even the degenerate one");
    CHECK(r.X == 52, "X moved");
    CHECK(r.Y == 60, "Y did not, because its roll is clamped to 0,0");
}

// -----------------------------------------------------------------------------
static void Test_ExhaustedSearchFallsBackToTheAimPoint()
{
    std::printf("when no roll works, the aim point is the last candidate\n");

    auto const band = Band(-5, 5, -5, 5);

    // Every roll moves the cell, and only the aim point is acceptable.
    FakeRoller roll{ {}, 0, 0, 3 };
    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll),
                                  [](int x, int y) { return x == 50 && y == 60; });

    CHECK(r.Fire, "it still fires");
    CHECK(r.X == 50 && r.Y == 60, "from the unscattered cell");
    CHECK(r.UsedOriginal, "and says it fell back");
    CHECK(!r.Shifted, "which is not a shift");
    CHECK(r.Attempts == ScatterAttempts, "after exhausting every attempt");
    CHECK(roll.Draws == 2 * ScatterAttempts, "two draws per attempt throughout");
}

// -----------------------------------------------------------------------------
static void Test_ARollBackOntoTheAimPointIsNotAFallBack()
{
    std::printf("rolling 0,0 is an ordinary result, not an exhausted search\n");

    auto const band = Band(-5, 5, -5, 5);
    FakeRoller roll{ {}, 0, 0, 0 };   // every roll is zero

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll), AcceptAll);
    CHECK(r.Fire, "it fires");
    CHECK(r.X == 50 && r.Y == 60, "from the aim point");
    CHECK(!r.UsedOriginal, "but as a rolled result, not a fall-back");
    CHECK(r.Attempts == 1, "on the first attempt");
    CHECK(!r.Shifted, "and nothing moved");
}

// -----------------------------------------------------------------------------
static void Test_RefusesWhenEvenTheAimPointFails()
{
    std::printf("the fall-back is held to the same checks, so it can refuse\n");

    auto const band = Band(-5, 5, -5, 5);
    FakeRoller roll{ {}, 0, 0, 3 };

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll), AcceptNone);
    CHECK(!r.Fire, "the launch is refused");
    CHECK(r.UsedOriginal, "after falling back");
    CHECK(r.Attempts == ScatterAttempts, "and exhausting the rolls first");
}

// -----------------------------------------------------------------------------
static void Test_InactiveBandResolvesToTheAimPoint()
{
    std::printf("an inactive band draws nothing and fires as aimed\n");

    ScatterBand none;
    FakeRoller roll{ { 5, 5 }, 0, 0, 0 };

    auto const r = ResolveScatter(none, 50, 60, Wide(), std::ref(roll), AcceptNone);
    CHECK(r.Fire, "it fires -- the random stage does not apply");
    CHECK(r.X == 50 && r.Y == 60, "from the aim point");
    CHECK(roll.Draws == 0, "and consumed no randomness at all");
    CHECK(r.Attempts == 0, "having made no attempt");
}

// -----------------------------------------------------------------------------
static void Test_AttemptCountIsHonoured()
{
    std::printf("the attempt cap is respected\n");

    auto const band = Band(-5, 5, -5, 5);
    FakeRoller roll{ {}, 0, 0, 3 };

    auto const r = ResolveScatter(band, 50, 60, Wide(), std::ref(roll),
                                  AcceptNone, 3);
    CHECK(r.Attempts == 3, "stopped at the cap given");
    CHECK(roll.Draws == 6, "with two draws each");
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
    Test_SidesAndDefaults();
    Test_ProbePlanning();
    Test_ProbePlanningCollapsesAtTheTargetStage();
    Test_RexsCase_WideXFreeNarrowYConstrained();
    Test_DesignatorAndInhibitorPlanIndependently();
    Test_ResolveTakesTheFirstAcceptableRoll();
    Test_ResolveRerollsPastRefusedCells();
    Test_BothAxesDrawnEvenWhenDegenerate();
    Test_ExhaustedSearchFallsBackToTheAimPoint();
    Test_ARollBackOntoTheAimPointIsNotAFallBack();
    Test_RefusesWhenEvenTheAimPointFails();
    Test_InactiveBandResolvesToTheAimPoint();
    Test_AttemptCountIsHonoured();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

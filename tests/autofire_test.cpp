/*
 * Off-target tests for auto-fire conditions.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/autofire_test.cpp -o af && ./af
 */
#include <SW/AutoFire.h>

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
    AutoFireRule MoneyRuleAt(int lo, int hi)
    {
        AutoFireRule r;
        r.Enabled     = true;
        r.OnMoneyMin  = lo;
        r.OnMoneyMax  = hi;
        return r;
    }

    AutoFireInputs At(int frame, int money = -1, bool charged = true)
    {
        AutoFireInputs in;
        in.Frame   = frame;
        in.Money   = money;
        in.Charged = charged;
        return in;
    }
}

// -----------------------------------------------------------------------------
static void Test_InertWhenUnconfigured()
{
    std::printf("unconfigured rules never fire\n");

    AutoFireRule none;
    AutoFireState st;
    CHECK(!none.Active(), "a default rule is inactive");
    CHECK(!ShouldAutoFire(none, At(100, 50000), st), "inactive rule does not fire");

    // Enabled but with no condition is still inert -- "yes" alone must not mean
    // "fire constantly", which is what Antares' bare SW.AutoFire already does.
    AutoFireRule bare;
    bare.Enabled = true;
    CHECK(!bare.Active(), "Enabled with no condition is inert");
    CHECK(!ShouldAutoFire(bare, At(100), st), "...and does not fire");
}

// -----------------------------------------------------------------------------
static void Test_MomentaryConditions()
{
    std::printf("momentary conditions\n");

    AutoFireRule sw;
    sw.Enabled   = true;
    sw.OnSWFired = { 7 };

    AutoFireState st;
    auto in = At(100);
    CHECK(!ShouldAutoFire(sw, in, st), "nothing fired -> no launch");

    in.SWFired = true;
    CHECK(ShouldAutoFire(sw, in, st), "a watched SW firing triggers us");

    // Momentary means momentary: the next frame, with the flag cleared, is quiet.
    auto quiet = At(101);
    CHECK(!ShouldAutoFire(sw, quiet, st), "does not keep firing after the event");

    CHECK(sw.WatchesSW(7), "watches the listed SW");
    CHECK(!sw.WatchesSW(8), "ignores an unlisted SW");

    AutoFireRule def;
    def.Enabled  = true;
    def.OnDefeat = true;
    AutoFireState st2;
    auto d = At(200);
    d.Defeated = true;
    CHECK(ShouldAutoFire(def, d, st2), "a defeat triggers");

    AutoFireRule vic;
    vic.Enabled   = true;
    vic.OnVictory = true;
    AutoFireState st3;
    auto v = At(300);
    v.Victory = true;
    CHECK(ShouldAutoFire(vic, v, st3), "victory triggers");
    CHECK(!ShouldAutoFire(vic, d, st3), "a defeat does not trigger a victory rule");
}

// -----------------------------------------------------------------------------
static void Test_MoneyFiresOnRisingEdgeOnly()
{
    std::printf("money band: rising edge only\n");

    // ⚠ THE BUG THIS GUARDS. A money band stays true for thousands of frames.
    // Firing on the level would launch every single frame.
    auto rule = MoneyRuleAt(5000, 9000);
    AutoFireState st;

    CHECK(!ShouldAutoFire(rule, At(1, 1000), st), "below the band: quiet");
    CHECK(ShouldAutoFire(rule, At(2, 6000), st),  "entering the band FIRES");

    int extra = 0;
    for (int f = 3; f < 100; ++f)
        if (ShouldAutoFire(rule, At(f, 6000), st))
            ++extra;
    CHECK(extra == 0, "staying in the band fires exactly ONCE, not every frame");

    // Leaving re-arms it.
    CHECK(!ShouldAutoFire(rule, At(100, 20000), st), "leaving above the band: quiet");
    CHECK(ShouldAutoFire(rule, At(101, 6000), st),   "re-entering fires again");

    // An open-ended minimum behaves the same way.
    AutoFireRule minOnly;
    minOnly.Enabled    = true;
    minOnly.OnMoneyMin = 10000;
    AutoFireState st2;
    CHECK(!ShouldAutoFire(minOnly, At(1, 9999), st2), "under the minimum: quiet");
    CHECK(ShouldAutoFire(minOnly, At(2, 10000), st2), "reaching it fires");
    CHECK(!ShouldAutoFire(minOnly, At(3, 999999), st2), "getting richer does not re-fire");
}

// -----------------------------------------------------------------------------
static void Test_BlockedEdgeIsConsumedNotDeferred()
{
    std::printf("a blocked rising edge is consumed, not deferred\n");

    // ⚠ If the edge were remembered while uncharged, the shot would go off at
    // some arbitrary later moment -- in game, a superweapon firing for no
    // visible reason. The edge must be spent on the frame it happened.
    auto rule = MoneyRuleAt(5000, -1);
    AutoFireState st;

    CHECK(!ShouldAutoFire(rule, At(10, 6000, /*charged*/ false), st),
          "edge while uncharged does not fire");
    CHECK(!ShouldAutoFire(rule, At(11, 6000, /*charged*/ true), st),
          "and is NOT replayed once charged -- the edge was consumed");

    // It re-arms properly on the next genuine transition.
    CHECK(!ShouldAutoFire(rule, At(12, 100, true), st), "drop out of the band");
    CHECK(ShouldAutoFire(rule, At(13, 6000, true), st), "a NEW edge fires normally");
}

// -----------------------------------------------------------------------------
static void Test_Cooldown()
{
    std::printf("cooldown\n");

    AutoFireRule r;
    r.Enabled   = true;
    r.OnSWFired = { 7 };
    r.Cooldown  = 100;

    AutoFireState st;
    auto in = At(1000);
    in.SWFired = true;

    CHECK(ShouldAutoFire(r, in, st), "first trigger fires");

    auto soon = At(1050);
    soon.SWFired = true;
    CHECK(!ShouldAutoFire(r, soon, st), "a second trigger inside the cooldown is refused");

    auto later = At(1100);
    later.SWFired = true;
    CHECK(ShouldAutoFire(r, later, st), "exactly at the cooldown it fires again");

    // A new scenario restarts the frame counter; the rule must not be locked out.
    auto restarted = At(5);
    restarted.SWFired = true;
    CHECK(ShouldAutoFire(r, restarted, st),
          "a backwards frame jump (new match) does not lock the rule out");
}

// -----------------------------------------------------------------------------
static void Test_ChargeGate()
{
    std::printf("charge gate\n");

    AutoFireRule r;
    r.Enabled   = true;
    r.OnSWFired = { 7 };

    AutoFireState st;
    auto in = At(10, -1, /*charged*/ false);
    in.SWFired = true;
    CHECK(!ShouldAutoFire(r, in, st), "uncharged: refused by default");

    r.RequireCharged = false;
    AutoFireState st2;
    CHECK(ShouldAutoFire(r, in, st2), "RequireCharged=no fires regardless");
}

int main()
{
    std::printf("SuperWeaponExt auto-fire conditions\n\n");

    Test_InertWhenUnconfigured();
    Test_MomentaryConditions();
    Test_MoneyFiresOnRisingEdgeOnly();
    Test_BlockedEdgeIsConsumedNotDeferred();
    Test_Cooldown();
    Test_ChargeGate();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

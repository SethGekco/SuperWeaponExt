/*
 * Off-target tests for superweapon financial requirements.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/money_test.cpp -o mt && ./mt
 */
#include <SW/Money.h>

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
    constexpr bool AsHuman = true;
    constexpr bool AsAI    = false;
}

// -----------------------------------------------------------------------------
static void Test_InertWhenUnset()
{
    std::printf("unconfigured rules do nothing\n");

    MoneyRule none;
    CHECK(!none.Active(), "an unconfigured rule is inactive");
    CHECK(none.Allows(0, AsHuman), "a broke house may fire when nothing is configured");
    CHECK(none.Allows(0, AsAI),    "same for the AI");
    CHECK(none.CostFor(AsHuman) == 0, "no cost by default");
}

// -----------------------------------------------------------------------------
static void Test_Cost()
{
    std::printf("cost as an affordability floor\n");

    MoneyRule r;
    r.Both.Cost = 2000;

    CHECK(r.Active(), "a cost activates the rule");
    CHECK(!r.Allows(1999, AsHuman), "cannot fire one credit short");
    CHECK(r.Allows(2000, AsHuman),  "exactly the cost is affordable");
    CHECK(r.Allows(9999, AsHuman),  "more than the cost is fine");

    // The AI is held to the same bar unless told otherwise.
    CHECK(!r.Allows(1999, AsAI), "the AI cannot fire what it cannot afford either");
}

// -----------------------------------------------------------------------------
static void Test_MinAndMax()
{
    std::printf("minimum / maximum credit thresholds\n");

    MoneyRule min;
    min.Both.Min = 5000;
    CHECK(!min.Allows(4999, AsAI), "below the minimum is refused");
    CHECK(min.Allows(5000, AsAI),  "exactly the minimum passes");

    // Min is money KEPT, not spent -- nothing is deducted by a minimum alone.
    CHECK(min.CostFor(AsAI) == 0, "a minimum does not imply a cost");

    MoneyRule max;
    max.Both.Max = 1000;
    CHECK(max.Allows(1000, AsHuman),  "exactly the maximum passes");
    CHECK(!max.Allows(1001, AsHuman), "above the maximum is refused");

    // Max=0 must mean "only while completely broke", NOT "no maximum". This is
    // why <0 is the unset marker rather than 0.
    MoneyRule broke;
    broke.Both.Max = 0;
    CHECK(broke.Active(), "Max=0 is a real setting, not an unset one");
    CHECK(broke.Allows(0, AsHuman),  "Max=0 allows a broke house");
    CHECK(!broke.Allows(1, AsHuman), "Max=0 refuses a house with any credits");

    // A band: usable only in the mid-game.
    MoneyRule band;
    band.Both.Min = 2000;
    band.Both.Max = 8000;
    CHECK(!band.Allows(1999, AsHuman), "below the band");
    CHECK(band.Allows(5000, AsHuman),  "inside the band");
    CHECK(!band.Allows(8001, AsHuman), "above the band");
}

// -----------------------------------------------------------------------------
static void Test_PerControllerOverrides()
{
    std::printf("human / AI overrides\n");

    MoneyRule r;
    r.Both.Cost = 1000;
    r.AI.Cost   = 3000;   // the AI pays more

    CHECK(r.CostFor(AsHuman) == 1000, "human uses the general cost");
    CHECK(r.CostFor(AsAI)    == 3000, "AI uses its override");

    CHECK(r.Allows(1500, AsHuman),  "human affords the general cost");
    CHECK(!r.Allows(1500, AsAI),    "AI cannot afford its higher cost");

    // Teaching the AI restraint without touching the human at all.
    MoneyRule restraint;
    restraint.AI.Min = 10000;
    CHECK(restraint.Allows(0, AsHuman),   "a human-side rule is untouched");
    CHECK(!restraint.Allows(9999, AsAI),  "the AI waits until it is comfortable");
    CHECK(restraint.Allows(10000, AsAI),  "and fires once it is");
}

// -----------------------------------------------------------------------------
static void Test_ResolutionIsPerField()
{
    std::printf("per-field fallback\n");

    // ⚠ The trap this guards: setting ONE override field must not discard the
    // other general fields. A whole-spec fallback would drop Min here.
    MoneyRule r;
    r.Both.Min  = 5000;
    r.Both.Cost = 1000;
    r.AI.Cost   = 2000;   // only the cost is overridden for the AI

    const MoneySpec ai = r.Resolve(AsAI);
    CHECK(ai.Cost == 2000, "the overridden field wins");
    CHECK(ai.Min  == 5000, "the NON-overridden field still falls back to the general value");

    CHECK(!r.Allows(4999, AsAI), "the inherited minimum is still enforced for the AI");
    CHECK(r.Allows(5000, AsAI),  "and satisfied above it");
}

// -----------------------------------------------------------------------------
static void Test_NegativeCostIsIncome()
{
    std::printf("negative cost = income\n");

    // A negative cost pays the house instead. It must never be treated as an
    // affordability barrier -- a broke house can always accept money.
    MoneyRule r;
    r.Both.Cost = -1500;

    CHECK(r.Active(), "a negative cost still activates the rule");
    CHECK(r.Allows(0, AsHuman), "a broke house may fire a superweapon that PAYS it");
    CHECK(r.CostFor(AsHuman) == -1500, "the negative amount is preserved for the adapter");
}

int main()
{
    std::printf("SuperWeaponExt money requirements\n\n");

    Test_InertWhenUnset();
    Test_Cost();
    Test_MinAndMax();
    Test_PerControllerOverrides();
    Test_ResolutionIsPerField();
    Test_NegativeCostIsIncome();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

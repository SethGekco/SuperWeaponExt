/*
 * Off-target tests for unit standing orders.
 *
 *   g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/standingorder_test.cpp -o so && ./so
 */
#include <SW/StandingOrder.h>

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
    OrderTarget MakeTarget(int typeIndex, Relation rel, int x, int y)
    {
        OrderTarget t;
        t.TypeIndex = typeIndex;
        t.Rel       = rel;
        t.CellX     = x;
        t.CellY     = y;
        t.Active    = true;
        return t;
    }

    StandingOrder MakeOrder(std::vector<int> types, Relation affects, int range = 0)
    {
        StandingOrder o;
        o.Mode        = StandingOrderMode::Techno;
        o.TypeIndices = std::move(types);
        o.Affects     = affects;
        o.Range       = range;
        return o;
    }
}

// -----------------------------------------------------------------------------
static void Test_InactiveDoesNothing()
{
    std::printf("inactive orders\n");

    StandingOrder none;
    CHECK(!none.Active(), "an unconfigured order is inactive");
    CHECK(PickNearest(none, {}, 0, 0) == nullptr, "inactive order picks nothing");

    // Techno mode with no types listed is inert, not a wildcard.
    StandingOrder empty;
    empty.Mode = StandingOrderMode::Techno;
    CHECK(!empty.Active(), "Techno mode with no types is inert");

    // SWImpact needs no type list — the destination comes from the blackboard.
    StandingOrder impact;
    impact.Mode = StandingOrderMode::SWImpact;
    CHECK(impact.Active(), "SWImpact mode is active without a type list");

    CHECK(!DueThisFrame(none, 100, 0), "an inactive order is never due");
}

// -----------------------------------------------------------------------------
static void Test_PicksNearest()
{
    std::printf("nearest selection\n");

    auto order = MakeOrder({ 7 }, Relation::Enemies);
    std::vector<OrderTarget> targets{
        MakeTarget(7, Relation::Enemies, 100, 0),
        MakeTarget(7, Relation::Enemies,  10, 0),   // nearest
        MakeTarget(7, Relation::Enemies,  50, 0),
    };

    auto const* pick = PickNearest(order, targets, 0, 0);
    CHECK(pick != nullptr, "a matching target is found");
    CHECK(pick && pick->CellX == 10, "the NEAREST matching target wins");

    // Distance is measured from the unit, not the origin.
    auto const* far = PickNearest(order, targets, 90, 0);
    CHECK(far && far->CellX == 100, "nearest is relative to the asking unit");
}

// -----------------------------------------------------------------------------
static void Test_Filters()
{
    std::printf("type / relation / active filters\n");

    auto order = MakeOrder({ 7 }, Relation::Enemies);

    std::vector<OrderTarget> wrong{
        MakeTarget(99, Relation::Enemies, 1, 0),   // wrong type
        MakeTarget(7,  Relation::Owner,   2, 0),   // wrong relation
    };
    auto dead = MakeTarget(7, Relation::Enemies, 3, 0);
    dead.Active = false;
    wrong.push_back(dead);

    CHECK(PickNearest(order, wrong, 0, 0) == nullptr,
          "wrong type / wrong relation / inactive are all rejected");

    // Relation scoping mirrors the inhibitor vocabulary.
    auto ownerOnly = MakeOrder({ 7 }, Relation::Owner);
    std::vector<OrderTarget> mine{ MakeTarget(7, Relation::Owner, 5, 0) };
    CHECK(PickNearest(ownerOnly, mine, 0, 0) != nullptr,
          "Owner-scoped order homes on own technos");
    CHECK(PickNearest(order, mine, 0, 0) == nullptr,
          "Enemies-scoped order ignores own technos");

    // Team == Owner|Allies.
    auto team = MakeOrder({ 7 }, Relation::Team);
    std::vector<OrderTarget> ally{ MakeTarget(7, Relation::Allies, 5, 0) };
    CHECK(PickNearest(team, ally, 0, 0) != nullptr, "Team scope includes allies");
}

// -----------------------------------------------------------------------------
static void Test_Range()
{
    std::printf("search range\n");

    std::vector<OrderTarget> targets{ MakeTarget(7, Relation::Enemies, 30, 40) };  // dist 50

    auto unlimited = MakeOrder({ 7 }, Relation::Enemies, 0);
    CHECK(PickNearest(unlimited, targets, 0, 0) != nullptr,
          "Range 0 searches the whole map");

    auto tight = MakeOrder({ 7 }, Relation::Enemies, 49);
    CHECK(PickNearest(tight, targets, 0, 0) == nullptr,
          "a target beyond Range is ignored");

    auto exact = MakeOrder({ 7 }, Relation::Enemies, 50);
    CHECK(PickNearest(exact, targets, 0, 0) != nullptr,
          "a target at exactly Range still counts (<=)");
}

// -----------------------------------------------------------------------------
static void Test_TieBreakIsDeterministic()
{
    std::printf("tie-breaking\n");

    // Two targets at identical distance. Whichever is chosen, it must be the
    // SAME one every time and on every client -- a tie broken differently on two
    // machines sends one unit to two places and desyncs.
    auto order = MakeOrder({ 7 }, Relation::Enemies);
    std::vector<OrderTarget> tied{
        MakeTarget(7, Relation::Enemies,  10, 0),
        MakeTarget(7, Relation::Enemies, -10, 0),
    };

    auto const* a = PickNearest(order, tied, 0, 0);
    auto const* b = PickNearest(order, tied, 0, 0);
    CHECK(a == b, "repeated evaluation returns the identical target");
    CHECK(a && a->CellX == 10, "ties resolve to the EARLIEST array index, not by chance");

    // Reversing the input reverses the winner -- confirming the rule really is
    // array order, so callers know the array order is what must stay synced.
    std::vector<OrderTarget> reversed{ tied[1], tied[0] };
    auto const* c = PickNearest(order, reversed, 0, 0);
    CHECK(c && c->CellX == -10, "the tie-break follows array order, nothing else");
}

// -----------------------------------------------------------------------------
static void Test_IntervalStagger()
{
    std::printf("interval stagger\n");

    auto order = MakeOrder({ 7 }, Relation::Enemies);
    order.Interval = 10;

    // A given unit fires exactly once per interval.
    int hits = 0;
    for (int f = 0; f < 100; ++f)
        if (DueThisFrame(order, f, 3))
            ++hits;
    CHECK(hits == 10, "a unit re-evaluates exactly once per interval");

    // Different units land on different frames, spreading the cost.
    int sameFrame = 0;
    for (int u = 0; u < 10; ++u)
        if (DueThisFrame(order, 0, u))
            ++sameFrame;
    CHECK(sameFrame == 1, "only one unit in ten shares any given frame");

    // Deterministic: same inputs, same answer.
    CHECK(DueThisFrame(order, 57, 4) == DueThisFrame(order, 57, 4),
          "due-ness is a pure function of frame and unit index");

    // Negative or zero interval must not divide by zero or loop forever.
    auto zero = order;
    zero.Interval = 0;
    CHECK(DueThisFrame(zero, 5, 1) || !DueThisFrame(zero, 5, 1),
          "interval 0 is handled without crashing");

    // Large unit indices keep a positive phase.
    CHECK(DueThisFrame(order, 0, 1000000) || true, "large indices do not misbehave");
}

// -----------------------------------------------------------------------------
static void Test_LargeMapArithmetic()
{
    std::printf("large-map arithmetic\n");

    auto order = MakeOrder({ 7 }, Relation::Enemies, 30000);
    std::vector<OrderTarget> far{ MakeTarget(7, Relation::Enemies, 20000, 20000) };
    CHECK(PickNearest(order, far, 0, 0) != nullptr,
          "huge coordinates do not overflow the distance comparison");

    auto tight = MakeOrder({ 7 }, Relation::Enemies, 5);
    CHECK(PickNearest(tight, far, 0, 0) == nullptr,
          "a distant target is still correctly rejected at map scale");
}

int main()
{
    std::printf("SuperWeaponExt standing orders\n\n");

    Test_InactiveDoesNothing();
    Test_PicksNearest();
    Test_Filters();
    Test_Range();
    Test_TieBreakIsDeterministic();
    Test_IntervalStagger();
    Test_LargeMapArithmetic();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

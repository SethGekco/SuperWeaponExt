/*
 * Off-target tests for the SuperWeaponExt constraint core.
 *
 * Builds and runs on Linux — no YRpp, no Windows, no game. This is the only
 * part of the DLL that can be verified without a Windows CI run, so the rules
 * that actually decide whether a superweapon fires live in Constraint.h
 * specifically so they can be exercised here.
 *
 *   g++ -std=c++20 -Wall -Wextra -Isrc tests/constraint_test.cpp -o ct && ./ct
 */
#include <SW/Constraint.h>

#include <cstdio>
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
    // A plain enemy building at (10,10), range 5, powered and alive.
    Source MakeSource(int typeIndex, Relation rel, int x, int y, int fallbackRange)
    {
        Source s;
        s.TypeIndex     = typeIndex;
        s.Rel           = rel;
        s.CellX         = x;
        s.CellY         = y;
        s.Active        = true;
        s.Powered       = true;
        s.FallbackRange = fallbackRange;
        return s;
    }

    Rule MakeRule(std::vector<int> types, Relation affects)
    {
        Rule r;
        r.TypeIndices = std::move(types);
        r.Affects     = affects;
        return r;
    }

    const Rule kInactive{};
}

// -----------------------------------------------------------------------------
static void Test_InactiveRulesConstrainNothing()
{
    std::printf("inactive rules\n");
    std::vector<Source> sources{ MakeSource(1, Relation::Enemies, 10, 10, 5) };

    CHECK(Allows(kInactive, kInactive, sources, 10, 10),
          "no rules configured => always allowed, even on top of a techno");
    CHECK(Allows(kInactive, kInactive, {}, 0, 0),
          "no rules, no sources => allowed");
}

// -----------------------------------------------------------------------------
static void Test_InhibitorBlocksInsideRadius()
{
    std::printf("inhibitor radius\n");
    auto rule = MakeRule({ 1 }, Relation::Enemies);
    std::vector<Source> sources{ MakeSource(1, Relation::Enemies, 10, 10, 5) };

    CHECK(!Allows(rule, kInactive, sources, 10, 10), "dead centre is blocked");
    CHECK(!Allows(rule, kInactive, sources, 13, 14), "distance 5 exactly is blocked (<=)");
    CHECK(Allows(rule, kInactive, sources, 20, 20),  "far outside is allowed");

    // 3-4-5 triangle: (13,14) is exactly 5 away, (14,14) is sqrt(32)>5.
    CHECK(Allows(rule, kInactive, sources, 15, 15), "just outside the radius is allowed");
}

// -----------------------------------------------------------------------------
static void Test_RelationFiltering()
{
    std::printf("relation filtering (owner/team/ally/enemy)\n");

    // The wishlist item: an inhibitor that only counts when it is OUR OWN.
    auto ownerOnly = MakeRule({ 1 }, Relation::Owner);
    std::vector<Source> enemyOne{ MakeSource(1, Relation::Enemies, 0, 0, 5) };
    std::vector<Source> ownOne  { MakeSource(1, Relation::Owner,   0, 0, 5) };

    CHECK(Allows(ownerOnly, kInactive, enemyOne, 0, 0),
          "enemy techno does not trip an Owner-scoped inhibitor");
    CHECK(!Allows(ownerOnly, kInactive, ownOne, 0, 0),
          "own techno trips an Owner-scoped inhibitor");

    // Team == Owner|Allies, so an ally counts but an enemy does not.
    auto team = MakeRule({ 1 }, Relation::Team);
    std::vector<Source> allyOne{ MakeSource(1, Relation::Allies, 0, 0, 5) };
    CHECK(!Allows(team, kInactive, allyOne, 0, 0),  "ally trips a Team-scoped rule");
    CHECK(!Allows(team, kInactive, ownOne, 0, 0),   "owner trips a Team-scoped rule");
    CHECK(Allows(team, kInactive, enemyOne, 0, 0),  "enemy does not trip a Team-scoped rule");

    // Antares' hardcoded behaviour, reproduced via config rather than code.
    auto antaresLike = MakeRule({ 1 }, Relation::Enemies);
    CHECK(!Allows(antaresLike, kInactive, enemyOne, 0, 0), "Enemies scope matches Antares default");
}

// -----------------------------------------------------------------------------
static void Test_DesignatorRequiresPresence()
{
    std::printf("designator presence\n");
    auto designators = MakeRule({ 7 }, Relation::Owner);
    std::vector<Source> inRange { MakeSource(7, Relation::Owner, 10, 10, 5) };
    std::vector<Source> outRange{ MakeSource(7, Relation::Owner, 90, 90, 5) };

    CHECK(Allows(kInactive, designators, inRange, 11, 11),
          "a designator in range permits the shot");
    CHECK(!Allows(kInactive, designators, outRange, 11, 11),
          "no designator in range denies the shot");
    CHECK(!Allows(kInactive, designators, {}, 11, 11),
          "designators required but none exist => denied");
}

// -----------------------------------------------------------------------------
static void Test_PerSWRangeIndex()
{
    std::printf("per-SW range index\n");

    // The same building (type 3, TechnoType range 4) inhibiting two different
    // superweapons at two different radii — impossible in Antares/Phobos today,
    // because range lives on the TechnoType, not on the SW.
    std::vector<Source> sources{ MakeSource(3, Relation::Enemies, 0, 0, 4) };

    Rule tight = MakeRule({ 3 }, Relation::Enemies);
    tight.RangesByIndex = { 2 };

    Rule wide = MakeRule({ 3 }, Relation::Enemies);
    wide.RangesByIndex = { 20 };

    CHECK(Allows(tight, kInactive, sources, 6, 0), "cell 6 away is outside the tight override");
    CHECK(!Allows(wide, kInactive, sources, 6, 0), "cell 6 away is inside the wide override");

    // A negative entry means "fall back to the TechnoType range" (4 here).
    Rule fallback = MakeRule({ 3 }, Relation::Enemies);
    fallback.RangesByIndex = { -1 };
    CHECK(!Allows(fallback, kInactive, sources, 4, 0), "fallback uses the TechnoType range (4)");
    CHECK(Allows(fallback, kInactive, sources, 5, 0),  "fallback stops at the TechnoType range");

    // Short list: types beyond the supplied ranges fall back too.
    Rule shortList = MakeRule({ 3, 9 }, Relation::Enemies);
    shortList.RangesByIndex = { 2 };
    std::vector<Source> other{ MakeSource(9, Relation::Enemies, 0, 0, 4) };
    CHECK(!Allows(shortList, kInactive, other, 4, 0),
          "a type with no matching range entry falls back to its TechnoType range");
}

// -----------------------------------------------------------------------------
static void Test_VeterancyScaling()
{
    std::printf("veterancy scaling\n");
    RangeSpec spec;
    spec.Base    = 4;
    spec.Veteran = 8;
    spec.Elite   = 16;

    CHECK(spec.Resolve(Rank::Rookie, 99) == 4,  "rookie uses Base");
    CHECK(spec.Resolve(Rank::Veteran, 99) == 8, "veteran uses Veteran");
    CHECK(spec.Resolve(Rank::Elite, 99) == 16,  "elite uses Elite");

    // Tier inheritance: an unset Elite falls back to Veteran, then Base.
    RangeSpec partial;
    partial.Base = 4;
    partial.Veteran = 8;
    CHECK(partial.Resolve(Rank::Elite, 99) == 8, "unset Elite inherits Veteran");

    RangeSpec baseOnly;
    baseOnly.Base = 4;
    CHECK(baseOnly.Resolve(Rank::Elite, 99) == 4, "unset Elite+Veteran inherit Base");

    // Nothing configured at all => the engine's Sight, matching Antares.
    RangeSpec empty;
    CHECK(empty.Resolve(Rank::Rookie, 99) == 99, "unconfigured range defaults to Sight");
    CHECK(empty.Resolve(Rank::Elite, 99) == 99,  "unconfigured elite range defaults to Sight");

    // And the end-to-end effect: an elite inhibitor reaches further.
    auto rule = MakeRule({ 1 }, Relation::Enemies);
    auto rookie = MakeSource(1, Relation::Enemies, 0, 0, spec.Resolve(Rank::Rookie, 0));
    auto elite  = MakeSource(1, Relation::Enemies, 0, 0, spec.Resolve(Rank::Elite, 0));
    CHECK(Allows(rule, kInactive, { rookie }, 10, 0), "rookie inhibitor does not reach 10 cells");
    CHECK(!Allows(rule, kInactive, { elite }, 10, 0), "elite inhibitor does reach 10 cells");
}

// -----------------------------------------------------------------------------
static void Test_ActivityAndPower()
{
    std::printf("activity and power gating\n");
    auto rule = MakeRule({ 1 }, Relation::Enemies);

    auto dead = MakeSource(1, Relation::Enemies, 0, 0, 5);
    dead.Active = false;
    CHECK(Allows(rule, kInactive, { dead }, 0, 0), "an inactive techno does not inhibit");

    auto unpowered = MakeSource(1, Relation::Enemies, 0, 0, 5);
    unpowered.Powered = false;
    CHECK(Allows(rule, kInactive, { unpowered }, 0, 0),
          "an unpowered building does not inhibit (matches Antares)");

    auto lenient = rule;
    lenient.RequirePower = false;
    CHECK(!Allows(lenient, kInactive, { unpowered }, 0, 0),
          "RequirePower=no lets an unpowered building inhibit");

    auto zeroRange = MakeSource(1, Relation::Enemies, 0, 0, 0);
    CHECK(Allows(rule, kInactive, { zeroRange }, 0, 0),
          "range 0 disables the source (matches Antares' range>0 guard)");
}

// -----------------------------------------------------------------------------
static void Test_AnyWildcard()
{
    std::printf("Any wildcard\n");
    Rule any;
    any.Any     = true;
    any.Affects = Relation::Enemies;

    std::vector<Source> odd{ MakeSource(4242, Relation::Enemies, 0, 0, 5) };
    CHECK(any.Active(), "Any=yes with an empty list is an active rule");
    CHECK(!Allows(any, kInactive, odd, 0, 0), "Any matches a type not in the list");

    // ...but relation scoping still applies to the wildcard.
    std::vector<Source> friendly{ MakeSource(4242, Relation::Owner, 0, 0, 5) };
    CHECK(Allows(any, kInactive, friendly, 0, 0), "Any still respects AffectsHouse");
}

// -----------------------------------------------------------------------------
static void Test_CompositionIsAnd()
{
    std::printf("inhibitor+designator composition\n");
    auto inhibitors  = MakeRule({ 1 }, Relation::Enemies);
    auto designators = MakeRule({ 7 }, Relation::Owner);

    std::vector<Source> both{
        MakeSource(7, Relation::Owner,   0, 0, 10),   // designator covers the cell
        MakeSource(1, Relation::Enemies, 0, 0, 10),   // but an inhibitor also covers it
    };
    CHECK(!Allows(inhibitors, designators, both, 1, 1),
          "an inhibitor overrides a satisfied designator");

    std::vector<Source> designatorOnly{ MakeSource(7, Relation::Owner, 0, 0, 10) };
    CHECK(Allows(inhibitors, designators, designatorOnly, 1, 1),
          "designator satisfied and no inhibitor => allowed");
}

// -----------------------------------------------------------------------------
static void Test_NoOverflowAtMapScale()
{
    std::printf("large-map arithmetic\n");
    // MapSizeExt pushes maps to 700x700+; make sure the squared distance math
    // does not overflow or go negative at that scale.
    auto rule = MakeRule({ 1 }, Relation::Enemies);
    std::vector<Source> corner{ MakeSource(1, Relation::Enemies, 0, 0, 30000) };
    CHECK(!Allows(rule, kInactive, corner, 20000, 20000),
          "huge ranges still compare correctly (no int overflow)");

    std::vector<Source> small{ MakeSource(1, Relation::Enemies, 0, 0, 5) };
    CHECK(Allows(rule, kInactive, small, 20000, 20000),
          "distant cell with a small range is allowed");
}

int main()
{
    std::printf("SuperWeaponExt constraint core\n\n");

    Test_InactiveRulesConstrainNothing();
    Test_InhibitorBlocksInsideRadius();
    Test_RelationFiltering();
    Test_DesignatorRequiresPresence();
    Test_PerSWRangeIndex();
    Test_VeterancyScaling();
    Test_ActivityAndPower();
    Test_AnyWildcard();
    Test_CompositionIsAnd();
    Test_NoOverflowAtMapScale();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

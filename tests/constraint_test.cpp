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

// -----------------------------------------------------------------------------
static void Test_GrowthOverTime()
{
    std::printf("growth over match time\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Growth.PerMinute = 6;                       // +6 cells per minute
    std::vector<Source> src{ MakeSource(1, Relation::Enemies, 0, 0, 4) };

    EvalContext t0;   t0.Frames = 0;
    EvalContext t1;   t1.Frames = FramesPerMinute;       // 1 minute
    EvalContext t2;   t2.Frames = FramesPerMinute * 2;   // 2 minutes

    CHECK(EffectiveRange(rule, src[0], t0) == 4,  "at t=0 the base range is untouched");
    CHECK(EffectiveRange(rule, src[0], t1) == 10, "after 1 minute: 4 + 6");
    CHECK(EffectiveRange(rule, src[0], t2) == 16, "after 2 minutes: 4 + 12");

    // End to end: a cell 8 away is safe at the start and covered a minute later.
    CHECK(Allows(rule, kInactive, src, 8, 0, t0),  "cell 8 away is outside the starting radius");
    CHECK(!Allows(rule, kInactive, src, 8, 0, t1), "the grown radius now covers it");

    // Partial minutes scale linearly, floored.
    EvalContext half; half.Frames = FramesPerMinute / 2;
    CHECK(EffectiveRange(rule, src[0], half) == 7, "half a minute gives +3 (floored)");
}

// -----------------------------------------------------------------------------
static void Test_ShrinkAndClamps()
{
    std::printf("shrink + clamps\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Growth.PerMinute = -5;
    std::vector<Source> src{ MakeSource(1, Relation::Enemies, 0, 0, 20) };

    EvalContext t2; t2.Frames = FramesPerMinute * 2;
    CHECK(EffectiveRange(rule, src[0], t2) == 10, "shrinks 5/min: 20 - 10");

    // Without a floor it would go negative and disable the source entirely.
    EvalContext t5; t5.Frames = FramesPerMinute * 5;
    CHECK(EffectiveRange(rule, src[0], t5) <= 0, "unclamped shrink can reach zero");
    CHECK(Allows(rule, kInactive, src, 0, 0, t5), "a source shrunk to nothing stops inhibiting");

    rule.Growth.Min = 6;
    CHECK(EffectiveRange(rule, src[0], t5) == 6, "Min floors the FINAL range");
    CHECK(!Allows(rule, kInactive, src, 3, 0, t5), "the floored radius still inhibits");

    auto grow = MakeRule({ 1 }, Relation::Enemies);
    grow.Growth.PerMinute = 10;
    grow.Growth.Max = 12;
    EvalContext t9; t9.Frames = FramesPerMinute * 9;
    CHECK(EffectiveRange(grow, src[0], t9) == 12, "Max ceilings the FINAL range");

    // Symmetry: growth and shrink of equal magnitude move by equal amounts.
    auto up = MakeRule({ 1 }, Relation::Enemies);   up.Growth.PerMinute = 7;
    auto dn = MakeRule({ 1 }, Relation::Enemies);   dn.Growth.PerMinute = -7;
    EvalContext t3; t3.Frames = FramesPerMinute * 3;
    const int base = 30;
    auto s2 = MakeSource(1, Relation::Enemies, 0, 0, base);
    CHECK(EffectiveRange(up, s2, t3) - base == base - EffectiveRange(dn, s2, t3),
          "floor division keeps growth and shrink symmetric");
}

// -----------------------------------------------------------------------------
static void Test_RatioExistence()
{
    std::printf("ratio: existence count (Range=0)\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Ratio.TypeIndices = { 50 };
    rule.Ratio.Affects = Relation::Enemies;
    rule.Ratio.PerUnit = 3;
    rule.Ratio.Range = 0;                 // anywhere on the map

    auto src = MakeSource(1, Relation::Enemies, 0, 0, 5);

    EvalContext none;
    CHECK(EffectiveRange(rule, src, none) == 5, "no counted technos => base range");

    EvalContext two;
    two.RatioSources = {
        RatioSource{ 50, Relation::Enemies, 900, 900, true },   // far away
        RatioSource{ 50, Relation::Enemies,  -50, -50, true },
    };
    CHECK(EffectiveRange(rule, src, two) == 11,
          "Range=0 counts them wherever they are: 5 + 2*3");

    // Type and relation filters still apply to the counted set.
    EvalContext wrong;
    wrong.RatioSources = {
        RatioSource{ 99, Relation::Enemies, 0, 0, true },   // wrong type
        RatioSource{ 50, Relation::Owner,   0, 0, true },   // wrong relation
        RatioSource{ 50, Relation::Enemies, 0, 0, false },  // inactive
    };
    CHECK(EffectiveRange(rule, src, wrong) == 5,
          "wrong type / wrong relation / inactive are all ignored");
}

// -----------------------------------------------------------------------------
static void Test_RatioProximity()
{
    std::printf("ratio: proximity count (Range>0)\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Ratio.TypeIndices = { 50 };
    rule.Ratio.Affects = Relation::All;
    rule.Ratio.PerUnit = 4;
    rule.Ratio.Range = 10;                // within 10 cells OF THE INHIBITOR

    auto src = MakeSource(1, Relation::Enemies, 100, 100, 6);

    EvalContext ctx;
    ctx.RatioSources = {
        RatioSource{ 50, Relation::Enemies, 106, 108, true },   // dist 10 -> counts
        RatioSource{ 50, Relation::Enemies, 100, 111, true },   // dist 11 -> does not
        RatioSource{ 50, Relation::Enemies, 100, 100, true },   // same cell -> counts
    };
    CHECK(CountRatioFor(rule, src, ctx) == 2, "counts only what is within Range of the source");
    CHECK(EffectiveRange(rule, src, ctx) == 14, "6 + 2*4");

    // The radius is measured from the SOURCE, not from the target cell.
    auto far = MakeSource(1, Relation::Enemies, 0, 0, 6);
    CHECK(CountRatioFor(rule, far, ctx) == 0,
          "a different source at a different place counts nothing nearby");
}

// -----------------------------------------------------------------------------
static void Test_RatioCapAndNegative()
{
    std::printf("ratio: cap + negative scaling\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Ratio.TypeIndices = { 50 };
    rule.Ratio.Affects = Relation::All;
    rule.Ratio.PerUnit = 5;
    rule.Ratio.Max = 12;

    auto src = MakeSource(1, Relation::Enemies, 0, 0, 4);

    EvalContext many;
    for (int i = 0; i < 10; ++i)
        many.RatioSources.push_back(RatioSource{ 50, Relation::Enemies, 0, 0, true });

    CHECK(EffectiveRange(rule, src, many) == 16, "bonus capped at Max: 4 + 12, not 4 + 50");

    // Negative PerUnit shrinks, and Max caps the magnitude the same way.
    auto neg = rule;
    neg.Ratio.PerUnit = -5;
    CHECK(EffectiveRange(neg, src, many) <= 0, "negative ratio can shrink to nothing");
    CHECK(Allows(neg, kInactive, { src }, 0, 0, many),
          "shrunk to nothing => no longer inhibits");

    auto negTwo = neg;
    EvalContext two;
    two.RatioSources = { RatioSource{ 50, Relation::Enemies, 0, 0, true },
                         RatioSource{ 50, Relation::Enemies, 0, 0, true } };
    CHECK(EffectiveRange(negTwo, MakeSource(1, Relation::Enemies, 0, 0, 20), two) == 10,
          "negative ratio: 20 - 2*5");
}

// -----------------------------------------------------------------------------
static void Test_ModifiersCompose()
{
    std::printf("growth + ratio together\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    rule.Growth.PerMinute = 6;
    rule.Ratio.TypeIndices = { 50 };
    rule.Ratio.Affects = Relation::All;
    rule.Ratio.PerUnit = 2;
    rule.Growth.Max = 20;

    auto src = MakeSource(1, Relation::Enemies, 0, 0, 4);

    EvalContext ctx;
    ctx.Frames = FramesPerMinute;                                  // +6
    ctx.RatioSources = { RatioSource{ 50, Relation::Enemies, 0, 0, true },
                         RatioSource{ 50, Relation::Enemies, 0, 0, true } };  // +4

    CHECK(EffectiveRange(rule, src, ctx) == 14, "4 + 6 growth + 4 ratio");

    // The clamp bounds the combined result, not each term.
    ctx.Frames = FramesPerMinute * 5;                              // +30 -> way over
    CHECK(EffectiveRange(rule, src, ctx) == 20, "Max clamps growth AND ratio together");

    // Modifiers apply on top of a per-SW Ranges override too.
    auto ov = rule;
    ov.RangesByIndex = { 2 };
    ctx.Frames = FramesPerMinute;
    CHECK(EffectiveRange(ov, src, ctx) == 12, "override 2 + 6 growth + 4 ratio");
}

// -----------------------------------------------------------------------------
static void Test_ModifiersInertWhenUnset()
{
    std::printf("modifiers inert when unconfigured\n");

    auto rule = MakeRule({ 1 }, Relation::Enemies);
    auto src = MakeSource(1, Relation::Enemies, 0, 0, 7);

    EvalContext late;
    late.Frames = FramesPerMinute * 60;    // an hour in
    late.RatioSources = { RatioSource{ 50, Relation::Enemies, 0, 0, true } };

    CHECK(!rule.Growth.Active(), "unset growth is inactive");
    CHECK(!rule.Ratio.Active(),  "unset ratio is inactive");
    CHECK(EffectiveRange(rule, src, late) == 7,
          "an unconfigured rule is unaffected by time or nearby technos");

    // A ratio with types but no PerUnit is inert (and vice versa).
    auto half = rule;
    half.Ratio.TypeIndices = { 50 };
    CHECK(!half.Ratio.Active(), "types without PerUnit is inert");
    CHECK(EffectiveRange(half, src, late) == 7, "and changes nothing");
}

// -----------------------------------------------------------------------------
static void Test_GrowthDeterminism()
{
    std::printf("growth determinism\n");

    // Same inputs must give the same answer every time and on every client --
    // this is integer-only by construction, so this test is really guarding
    // against someone "simplifying" it to floating point later.
    GrowthSpec g; g.PerMinute = 7;
    for (int f = 0; f < 5000; f += 137)
    {
        const int a = g.DeltaAt(f);
        const int b = g.DeltaAt(f);
        if (a != b) { CHECK(false, "DeltaAt is not deterministic"); return; }
    }
    CHECK(true, "DeltaAt is stable across repeated evaluation");

    CHECK(g.DeltaAt(-100) == 0, "negative frame counts do not run growth backwards");
    CHECK(g.DeltaAt(0) == 0,    "frame 0 yields no growth");

    // No overflow at long-match frame counts.
    GrowthSpec big; big.PerMinute = 1000;
    CHECK(big.DeltaAt(900 * 600) > 0, "a 10-hour match does not overflow to negative");
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
    Test_GrowthOverTime();
    Test_ShrinkAndClamps();
    Test_RatioExistence();
    Test_RatioProximity();
    Test_RatioCapAndNegative();
    Test_ModifiersCompose();
    Test_ModifiersInertWhenUnset();
    Test_GrowthDeterminism();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

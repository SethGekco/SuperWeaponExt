/*
 * SuperWeaponExt — "use a different weapon against a designator/inhibitor".
 *
 * ============================================================================
 * WHY A VTABLE WRAP AND NOT A CODE HOOK
 * ============================================================================
 *
 * TechnoClass::WhatWeaponShouldIUse (`0x6F3330`, YRpp calls it SelectWeapon) is
 * the single most contested function in the game. Inside its body live NINE
 * Phobos hooks (Interceptor, MultiWeapon, ForceFire, ForceWeapon, Gattling,
 * Airstrike, IsLocomotor, the main one, AntiAir) and TWO Antares ones
 * (NoAmmoWeapon, Verses).
 *
 * Two tempting approaches are both wrong:
 *
 *   1. Hook the ENTRY and short-circuit to the epilogue. This is what Antares'
 *      (commented-out) TechnoClass_SelectWeapon did, and it would bypass all
 *      eleven of those hooks — silently breaking Gattling, MultiWeapon, ammo
 *      handling and Verses. It is also broken on its own terms: it jumped to
 *      `0x6F3813`, which disassembles as a bare `ret 0x4` reached only AFTER
 *      four pops and an `add esp,8` at `0x6F3807`–`0x6F3810`. Jumping there from
 *      the entry, before anything is pushed, returns on a corrupted stack.
 *
 *   2. Hook a `ret`. The function has several return paths, so this needs one
 *      hook per exit and still lands inside a heavily-hooked body.
 *
 * SelectWeapon is VIRTUAL (`YRpp/TechnoClass.h:227`), and `0x6F3330` is
 * referenced by exactly four .rdata vtable slots — one per TechnoClass subclass.
 * Replacing those slots gives a wrapper that CALLS the original, so every
 * framework hook inside still runs and decides first, and we only adjust the
 * answer afterwards. Same architecture as the Layer 3 cursor wrapper in
 * Hooks.Cursor.cpp, which is already verified in game.
 *
 * ⚠ The four slots are unclaimed: no Phobos or Antares source references them
 * and the hook registry has no rows for them. Verified before writing this.
 *
 * They are also confirmed to be REAL vtable entries rather than raw bytes that
 * happen to equal 0x6F3330: every neighbouring word at each of the four
 * addresses is a valid .text pointer, and all four are followed by the same next
 * virtual (0x6F3820). AircraftClass::AbsVTable is 0x7E22A4, and 0x7E2588 sits a
 * whole number of slots past it.
 *
 * ============================================================================
 * DETERMINISM
 * ============================================================================
 *
 * Weapon choice feeds combat, which is synced. This wrapper therefore reads only
 * type data and the target's identity — never the local player, the cursor, or
 * the view — so every client picks the same weapon. It is a pure function of
 * (firer type, target type, rules).
 */
#include <Ext/SWType/Body.h>
#include <Ext/TechnoType/Body.h>

#include <SuperWeaponTypeClass.h>
#include <CellClass.h>
#include <Fundamentals.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    // -------------------------------------------------------------------------
    // "Is this TechnoType configured as an inhibitor / designator anywhere?"
    //
    // Answered from a cache: this runs inside weapon selection, which is called
    // from targeting, cursor rendering, threat evaluation and firing, so walking
    // every superweapon per call is not an option.
    //
    // The cache is a union across all superweapons. The per-superweapon form
    // ("honor indexes") is answered directly from that SW's ext instead, since a
    // named SW needs no search.
    // -------------------------------------------------------------------------
    std::vector<int> g_inhibitorTypes;    // sorted unified type indices
    std::vector<int> g_designatorTypes;
    int g_builtForCount = -1;             // SuperWeaponTypeClass::Array.Count when built

    void BuildCache()
    {
        g_inhibitorTypes.clear();
        g_designatorTypes.clear();

        for (int i = 0; i < SuperWeaponTypeClass::Array.Count; ++i)
        {
            auto const pSW = SuperWeaponTypeClass::Array.GetItem(i);
            if (!pSW)
                continue;

            auto const pExt = SWTypeExt::ExtMap.Find(pSW);
            if (!pExt)
                continue;

            for (int idx : pExt->Inhibitors.TypeIndices)
                g_inhibitorTypes.push_back(idx);

            for (int idx : pExt->Designators.TypeIndices)
                g_designatorTypes.push_back(idx);
        }

        // Sorted + deduplicated so lookups are a binary search.
        for (auto* v : { &g_inhibitorTypes, &g_designatorTypes })
        {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }

        g_builtForCount = SuperWeaponTypeClass::Array.Count;

        Debug::Log("[SuperWeaponExt] weapon-select cache: %u inhibitor type(s), "
                   "%u designator type(s)\n",
                   static_cast<unsigned>(g_inhibitorTypes.size()),
                   static_cast<unsigned>(g_designatorTypes.size()));
    }

    void EnsureCache()
    {
        // Rebuilt when the superweapon array changes size, which is what a rules
        // reload (new scenario, different mod) looks like from here.
        if (g_builtForCount != SuperWeaponTypeClass::Array.Count)
            BuildCache();
    }

    bool InSorted(const std::vector<int>& v, int value)
    {
        return std::binary_search(v.begin(), v.end(), value);
    }

    // Does `pTargetType` act as the given role, optionally only for one named SW?
    bool ActsAs(TechnoTypeClass* pTargetType, bool wantInhibitor,
                SuperWeaponTypeClass* pScopedSW)
    {
        if (!pTargetType)
            return false;

        const int typeIndex = TechnoTypeExt::UnifiedIndex(pTargetType);
        if (typeIndex < 0)
            return false;

        // "Honor indexes": scoped to one superweapon's list rather than the union.
        if (pScopedSW)
        {
            auto const pSWExt = SWTypeExt::ExtMap.Find(pScopedSW);
            if (!pSWExt)
                return false;

            const auto& rule = wantInhibitor ? pSWExt->Inhibitors : pSWExt->Designators;
            return rule.CoversType(typeIndex);
        }

        EnsureCache();
        return InSorted(wantInhibitor ? g_inhibitorTypes : g_designatorTypes, typeIndex);
    }

    // -------------------------------------------------------------------------
    // "Is this techno standing inside an inhibitor's / designator's radius?"
    //
    // Built at most ONCE PER FRAME, and only when some loaded type actually asks
    // for a WhileNear rule — weapon selection is called from targeting, cursor
    // rendering, threat evaluation and firing, so a per-call scan of the techno
    // array would be indefensible. The per-frame list is small (one entry per
    // inhibitor/designator on the map), so the per-call test over it is cheap.
    //
    // ⚠ SIMPLIFICATION, deliberate and documented: the radius used here is the
    // TechnoType's veterancy-resolved InhibitorRange / DesignatorRange only. It
    // does NOT include the per-superweapon Ranges override, growth over time or
    // the proximity ratio, because those are properties of a (superweapon,
    // source) PAIR and weapon selection has no superweapon in hand. So a growing
    // inhibitor influences the launch veto out to its grown radius but influences
    // weapon choice out to its base radius. Anything else would require inventing
    // a superweapon to evaluate against.
    // -------------------------------------------------------------------------
    struct Influence
    {
        int  CellX;
        int  CellY;
        int  Range;      // cells
        bool IsInhibitor;
        bool IsDesignator;
    };

    std::vector<Influence> g_influence;
    int g_influenceFrame = -1;

    void EnsureInfluence()
    {
        const int frame = Unsorted::CurrentFrame;
        if (g_influenceFrame == frame)
            return;

        g_influenceFrame = frame;
        g_influence.clear();

        EnsureCache();

        for (int i = 0; i < TechnoClass::Array.Count; ++i)
        {
            TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
            if (!pTechno || !pTechno->IsAlive || pTechno->Health <= 0
                || pTechno->InLimbo)
            {
                continue;
            }

            auto const pType = pTechno->GetTechnoType();
            if (!pType)
                continue;

            const int typeIndex = TechnoTypeExt::UnifiedIndex(pType);
            const bool inhib = InSorted(g_inhibitorTypes, typeIndex);
            const bool desig = InSorted(g_designatorTypes, typeIndex);
            if (!inhib && !desig)
                continue;

            auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
            if (!pTypeExt)
                continue;

            const auto rank = pTechno->Veterancy.IsElite()   ? SWExt::Rank::Elite
                            : pTechno->Veterancy.IsVeteran() ? SWExt::Rank::Veteran
                                                             : SWExt::Rank::Rookie;

            // Sight is the engine's fallback when nothing was configured, the
            // same default the constraint evaluator uses.
            const int sight = pType->Sight;
            const int range = inhib ? pTypeExt->InhibitorRange.Resolve(rank, sight)
                                    : pTypeExt->DesignatorRange.Resolve(rank, sight);

            if (range <= 0)
                continue;

            const auto cell = CellClass::Coord2Cell(pTechno->GetCoords());
            g_influence.push_back(Influence{ cell.X, cell.Y, range, inhib, desig });
        }
    }

    bool StandingInInfluence(TechnoClass* pTechno, bool wantInhibitor)
    {
        if (!pTechno)
            return false;

        EnsureInfluence();

        const auto cell = CellClass::Coord2Cell(pTechno->GetCoords());

        for (auto const& inf : g_influence)
        {
            if (wantInhibitor ? !inf.IsInhibitor : !inf.IsDesignator)
                continue;

            const std::int64_t dx = static_cast<std::int64_t>(inf.CellX) - cell.X;
            const std::int64_t dy = static_cast<std::int64_t>(inf.CellY) - cell.Y;
            const std::int64_t r  = inf.Range;

            if (dx * dx + dy * dy <= r * r)
                return true;
        }
        return false;
    }

    // One log line per (firer type, target type) pair, ever.
    //
    // Weapon selection runs from targeting, cursor rendering, threat evaluation
    // and firing, so an unthrottled line would bury the log within seconds. But
    // some diagnostic is needed: whether the override fired is NOT reliably
    // visible in game, because the engine may pick a secondary weapon on its own
    // for unrelated reasons, so "it looked different" proves nothing.
    std::vector<std::pair<int, int>> g_logged;

    bool ShouldLog(int firerIdx, int targetIdx)
    {
        for (auto const& e : g_logged)
            if (e.first == firerIdx && e.second == targetIdx)
                return false;

        g_logged.emplace_back(firerIdx, targetIdx);
        return true;
    }

    // ⚠ CALLING CONVENTION — this crashed the game once already.
    //
    // A vtable slot is invoked as __thiscall: `this` in ECX, arguments pushed on
    // the stack, callee cleans. MSVC will not let you write __thiscall on a free
    // function, so the standard model is __fastcall with a DUMMY second
    // parameter to absorb EDX:
    //
    //     int __fastcall Wrapper(This* pThis, void* /*edx*/, Arg a)
    //
    // The first version of this file declared the wrapper __stdcall(pThis,
    // pTarget). That reads `this` off the STACK — so pThis was really pTarget and
    // pTarget was whatever lay past the frame — and, worse, a 2-parameter
    // __stdcall callee pops 8 bytes while the caller pushed only 4. Four bytes of
    // stack imbalance per call, and weapon selection is called constantly, so the
    // return address was destroyed almost immediately. It surfaced as EIP landing
    // inside .rdata (0x007FA9A0) right after a paradrop spawned planes.
    //
    // Hooks.Cursor.cpp had the correct pattern the whole time; this file cited it
    // as the model and then failed to copy the signature.
    using SelectWeaponFunc = int(__fastcall*)(TechnoClass*, void*, AbstractClass*);

    int __fastcall TechnoClass_SelectWeapon_Wrapper(TechnoClass* pThis,
                                                    void* /* unused EDX */,
                                                    AbstractClass* pTarget)
    {
        // Let the engine — and every framework hook inside it — decide first.
        // Our result is an override of a real answer, never a replacement for
        // running the function.
        const auto original = reinterpret_cast<SelectWeaponFunc>(0x6F3330);
        const int chosen = original(pThis, nullptr, pTarget);

        if (!pThis || !pTarget)
            return chosen;

        auto const pType = pThis->GetTechnoType();
        if (!pType)
            return chosen;

        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        if (!pTypeExt || !pTypeExt->WeaponVs.Active())
            return chosen;

        // Only technos can be inhibitors or designators; a cell target cannot.
        auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
        if (!pTargetTechno)
            return chosen;

        auto const pTargetType = pTargetTechno->GetTechnoType();
        if (!pTargetType)
            return chosen;

        const auto& rule = pTypeExt->WeaponVs;

        // Inhibitor is tested first so that a type listed as BOTH resolves
        // predictably rather than by iteration order.
        const bool vsInhib = rule.VsInhibitor >= 0
                          && ActsAs(pTargetType, true, rule.InhibitorSW);
        const bool vsDesig = !vsInhib
                          && rule.VsDesignator >= 0
                          && ActsAs(pTargetType, false, rule.DesignatorSW);

        // Firer-side, checked only if no target-side rule already applied. The
        // ordering is arbitrary but FIXED, so a type configuring both never
        // depends on evaluation order.
        const bool nearInhib = !vsInhib && !vsDesig
                            && rule.WhileNearInhibitor >= 0
                            && StandingInInfluence(pThis, true);
        const bool nearDesig = !vsInhib && !vsDesig && !nearInhib
                            && rule.WhileNearDesignator >= 0
                            && StandingInInfluence(pThis, false);

        if (!vsInhib && !vsDesig && !nearInhib && !nearDesig)
            return chosen;

        const int override_ = vsInhib   ? rule.VsInhibitor
                            : vsDesig   ? rule.VsDesignator
                            : nearInhib ? rule.WhileNearInhibitor
                                        : rule.WhileNearDesignator;

        const char* why = vsInhib   ? "inhibitor"
                        : vsDesig   ? "designator"
                        : nearInhib ? "near inhibitor"
                                    : "near designator";

        if (ShouldLog(TechnoTypeExt::UnifiedIndex(pType),
                      TechnoTypeExt::UnifiedIndex(pTargetType)))
        {
            Debug::Log("[SuperWeaponExt] %s vs %s (%s): weapon %d -> %d\n",
                       pType->ID, pTargetType->ID, why, chosen, override_);
        }

        return override_;
    }
}

// One slot per TechnoClass subclass. All four resolve to 0x6F3330 in a stock
// binary and none is claimed by Phobos, Antares or anything in the registry.
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2588, TechnoClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E41A0, TechnoClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E8F78, TechnoClass_SelectWeapon_Wrapper);
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F4C44, TechnoClass_SelectWeapon_Wrapper);

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
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <algorithm>
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

        if (!vsInhib && !vsDesig)
            return chosen;

        const int override_ = vsInhib ? rule.VsInhibitor : rule.VsDesignator;

        if (ShouldLog(TechnoTypeExt::UnifiedIndex(pType),
                      TechnoTypeExt::UnifiedIndex(pTargetType)))
        {
            Debug::Log("[SuperWeaponExt] %s vs %s (%s): weapon %d -> %d\n",
                       pType->ID, pTargetType->ID,
                       vsInhib ? "inhibitor" : "designator", chosen, override_);
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

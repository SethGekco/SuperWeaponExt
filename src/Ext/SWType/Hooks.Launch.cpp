/*
 * SuperWeaponExt — Layer 1: the authoritative launch veto.
 *
 * ============================================================================
 * HouseClass::Fire_SW @ 0x4FAE50
 * ============================================================================
 *
 * WHY THIS ADDRESS. Every superweapon launch in the game funnels through this
 * one function — 17 call sites, verified against a fresh disassembly:
 *
 *   0x4C78F3            RespondToEvent(SpecialPlace) — the network-synced
 *                       player-click path
 *   0x509ACD..0x50A480  HouseClass AI superweapon firing (10 sites, one per
 *                       vanilla SW)
 *   0x6EFDB4..0x6F030D  AI script / team-mission SW actions (5 sites)
 *   0x4FAE3F            internal tail-call
 *
 * The FRAMEWORK paths land here too, which is the whole point:
 *   - Antares' SWTypeExt::TryFire ends in pOwner->Fire_SW(...)
 *     (Hooks.Targeting.cpp:619), and YRpp's Fire_SW is JMP_THIS(0x4FAE50) —
 *     a direct call to this game address, not to anything inside Antares.
 *   - Phobos' SW-sidebar button and trigger actions 505/506 queue
 *     EventType::SpecialPlace, which arrives via 0x4C78F3.
 *
 * So one hook covers vanilla, Antares manual fire, Antares AI targeting, Phobos
 * sidebar, Phobos triggers and map triggers — without contending for a single
 * address any framework owns.
 *
 * REGISTRY STATUS: the ENTRY is unhooked by all six frameworks in the YR Hook
 * Encyclopedia (Ares, Antares, Phobos, Kratos, CnCNet-Spawner, AggressiveStance).
 * Antares takes 0x4FAE72 — that is +0x22 *inside* the function
 * (HouseClass_SWFire_PreDependent) and is not on the path from the entry.
 *
 * DESYNC PROPERTY: this sits downstream of the event queue, so every client
 * evaluates the veto on the same frame against the same game state. That is
 * inherited from the position, not from anything we do — which is exactly why
 * this layer, and not the cursor layer, is the authoritative one.
 *
 * ----------------------------------------------------------------------------
 * Stolen bytes (0x7), from the disassembly:
 *     4fae50:  53              push %ebx
 *     4fae51:  8b d9           mov  %ecx,%ebx
 *     4fae53:  8b 4c 24 08     mov  0x8(%esp),%ecx
 * 1 + 2 + 4 = 7, landing on an instruction boundary.
 *
 * At entry (nothing pushed yet):
 *     ECX      = HouseClass* this
 *     [esp+4]  = int idxSW
 *     [esp+8]  = CellStruct* pCoords
 *
 * ABORT PATH — the trap worth remembering. Do NOT jump to the real epilogue at
 * 0x4FAEED; it is `pop edi; pop esi; pop ebp; mov al,1; pop ebx; ret 8` and
 * would pop four registers that were never pushed. Jump to 0x4FAEF3 instead,
 * which is a bare `ret $8` — the correct cleanup for
 * __thiscall bool Fire_SW(int, CellStruct const&) from a pristine entry stack.
 */
#include "Body.h"

#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

DEFINE_HOOK(0x4FAE50, HouseClass_Fire_SW_ConstraintVeto, 0x7)
{
    enum { Continue = 0, Deny = 0x4FAEF3 };

    GET(HouseClass*, pThis, ECX);
    GET_STACK(int, idxSW, 0x4);
    GET_STACK(CellStruct*, pCoords, 0x8);

    if (!pThis || !pCoords)
        return Continue;

    SuperClass* const pSuper = pThis->Supers.GetItemOrDefault(idxSW);
    if (!pSuper || !pSuper->Type)
        return Continue;

    auto const pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

    // Not our superweapon to police — leave it entirely alone. This early-out
    // is what keeps a mod that never touches SWExt.* tags at zero added cost.
    if (!pExt || !pExt->IsConfigured())
        return Continue;

    if (pExt->AllowsFireAt(pThis, *pCoords))
        return Continue;

    Debug::Log("[SuperWeaponExt] denied %s for house %d at (%d,%d): "
               "inhibitor/designator constraints not met\n",
               pSuper->Type->ID, pThis->ArrayIndex, pCoords->X, pCoords->Y);

    // Fire_SW returns bool; report "did not fire".
    R->AL(0);
    return Deny;
}

// TODO(Layer 2, FINDINGS.md §6): click veto at 0x4AC21C — the convergence point
// both Antares (returning from its 0x4AC20C hook) and vanilla reach. Stolen
// bytes 0x6 (`mov edx,[eax+0x98]`), veto by returning 0x4AC294. Cosmetic +
// latency only; Layer 1 above is what actually enforces. Both layers must be
// driven by SWTypeExt::ExtData::AllowsFireAt so they can never disagree.
//
// TODO(Layer 3, FINDINGS.md §6): cursor veto at 0x6CEF80 — the real entry of
// SuperWeaponTypeClass::GetAction (0x6CEF73..0x6CEF7F are alignment nops).
// Antares hooks 0x6CEF84, four bytes later, leaving the 5-byte prologue free.
// Return 0 to allow so Antares still evaluates its own constraints; to deny,
// set EAX and jump 0x6CEFDB (bare `ret $8`). UNRESOLVED: which disallow code to
// write — Antares uses SuperWeaponDisallowed=0x7E (src/Misc/Actions.h:10) for
// its own SW types, vanilla returns 0x46 at 0x6CEFCA. Determine from the SW's
// type at runtime; do not guess.
//
// TODO(standing orders, FINDINGS.md §3b): record the per-house "last SW impact
// cell" here — this hook is the one place every launch is guaranteed to pass.
// Deliberately NOT done yet: that blackboard is session state and must be
// save/load serialized before it ships, or a mid-game save desyncs on reload.

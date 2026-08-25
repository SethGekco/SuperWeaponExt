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

#include <DisplayClass.h>
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
    if (!pExt || (!pExt->IsConfigured()
                  && !pExt->ParaDrop.Enabled
                  && !pExt->KeepSelectedAfterFire))
    {
        return Continue;
    }

    if (pExt->IsConfigured() && !pExt->AllowsFireAt(pThis, *pCoords))
    {
        Debug::Log("[SuperWeaponExt] denied %s for house %d at (%d,%d): "
                   "inhibitor/designator constraints not met\n",
                   pSuper->Type->ID, pThis->ArrayIndex, pCoords->X, pCoords->Y);

        R->AL(0);
        return Deny;
    }

    // OWNED PARADROP.
    //
    // ⚠ Why here and not at SuperClass::Launch (0x6CC390), which would be the
    // natural seat: Antares hooks 0x6CC390 and dispatches its own SW_ParaDrop
    // from there — and so do Ares and Phobos, three consumers on one address.
    // Syringe would chain us behind Antares, which returns "handled" for a
    // ParaDrop type, so our handler would never run. Injection order decides it,
    // which is not something to build on.
    //
    // Fire_SW's entry is upstream of all of that, so aborting here is the only
    // way to guarantee Antares' copy does not also fire.
    //
    // Sits AFTER the constraint check on purpose: an owned paradrop still obeys
    // inhibitors and designators.
    if (pExt->ParaDrop.Enabled
        && SWTypeExt::RunOwnedParaDrop(pSuper->Type, pThis, *pCoords))
    {
        // Aborting skips SuperClass::ClickFire, which is what would normally
        // spend the charge — so without this the superweapon would stay ready
        // and could be fired every frame. Reset() is the engine's own "put this
        // back on the clock" (the same one the spy-infiltration trigger action
        // uses), so the recharge behaves as the modder configured it.
        pSuper->Reset();

        // Aborting also skips the DESELECT. Unsorted::CurrentSWType (0x8809A0,
        // == DisplayClass::Instance.CurrentSWTypeIndex) is what keeps the cursor
        // armed, and the engine clears it from inside SuperClass::Launch — at
        // eleven separate `movl $-1` sites, one per superweapon action branch.
        // We never reach any of them, so without this the cursor stays holding
        // the superweapon after it has already fired.
        //
        // ⚠ Guarded on CurrentPlayer. Fire_SW runs on EVERY client (it is
        // downstream of the event queue), so clearing unconditionally would
        // reset the local cursor whenever a *remote* player fired. The pending-SW
        // index is local UI state, so touching it per-client is desync-safe —
        // but only if we touch the right client's.
        // SWExt.KeepSelectedAfterFire=yes simply skips this, leaving the cursor
        // armed so a fast-recharging superweapon can be fired again without
        // going back to the cameo.
        if (pThis == HouseClass::CurrentPlayer && !pExt->KeepSelectedAfterFire)
            DisplayClass::Instance.CurrentSWTypeIndex = -1;

        R->AL(1);      // report "it fired"
        return Deny;   // "Deny" here means "skip the engine's own launch"
    }

    // For every OTHER superweapon we let the engine launch normally, so the
    // engine also performs its own deselect — the eleven `movl $-1` sites inside
    // SuperClass::Launch. Rather than suppress those (Antares owns 0x6CC390, so
    // we cannot), re-arm the cursor on the next frame.
    //
    // Deliberately a ONE-SHOT request: re-arming every frame would fight the
    // player right-clicking to put the superweapon away.
    if (pExt->KeepSelectedAfterFire && pThis == HouseClass::CurrentPlayer)
        SWTypeExt::RequestKeepSelected(idxSW);

    return Continue;
}

// TODO(standing orders, FINDINGS.md §3b): record the per-house "last SW impact
// cell" here — this hook is the one place every launch is guaranteed to pass.
// Deliberately NOT done yet: that blackboard is session state and must be
// save/load serialized before it ships, or a mid-game save desyncs on reload.

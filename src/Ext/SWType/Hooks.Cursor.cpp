/*
 * SuperWeaponExt — Layers 2 and 3: stop the cursor lying.
 *
 * Layer 1 (Hooks.Launch.cpp) is authoritative and network-synced, but it acts at
 * the moment of launch. Without these two layers a blocked cell still shows an
 * "allowed" cursor and the click is silently eaten. These layers are cosmetic +
 * latency only; all three call the SAME predicate so they cannot disagree.
 *
 * ============================================================================
 * Layer 2 — DisplayClass::LeftMouseButtonUp @ 0x4AC21C
 * ============================================================================
 *
 * Antares hooks 0x4AC20C and returns to either 0x4AC21C (a superweapon was
 * resolved; EAX = SuperWeaponTypeClass*) or 0x4AC294 (none). Vanilla's own
 * `call FindFirstOfAction; test eax,eax; je 0x4AC294` converges on exactly the
 * same two addresses. So 0x4AC21C is the convergence point, it is unhooked by
 * every framework, and a hook there runs on the Antares path AND the vanilla
 * path — after either has decided.
 *
 *   4ac20c:  mov  0x9c(%esp),%ecx      <-- Antares' hook
 *   4ac213:  call 0x6ceeb0             <-- FindFirstOfAction
 *   4ac218:  test %eax,%eax
 *   4ac21a:  je   0x4ac294
 *   4ac21c:  mov  0x98(%eax),%edx      <-- OUR hook (6 stolen bytes)
 *   4ac222:  mov  0x94(%esp),%ecx      <-- the target cell
 *   ...      builds EventClass(house, SpecialPlace, swIdx, cell)
 *   4ac294:  <no-superweapon path>
 *
 * ESP is unchanged between 0x4AC21C and 0x4AC222, so the cell the game is about
 * to put in the event is readable at [esp+0x94] from our hook.
 *
 * ============================================================================
 * Layer 3 — SuperWeaponTypeClass::GetAction, via VTABLE slot 0x7F40FC
 * ============================================================================
 *
 * ⚠ NOT a code hook. An earlier draft planned to hook the function prologue at
 * 0x6CEF80, on the belief that Antares hooking 0x6CEF84 left five free bytes.
 * It does not — the prologue is only FOUR bytes:
 *
 *   6cef80:  56           push %esi        (1)
 *   6cef81:  57           push %edi        (1)
 *   6cef82:  8b f9        mov  %ecx,%edi   (2)
 *   6cef84:  83 bf ...    cmpl $0xa,...    <-- Antares' hook starts here
 *
 * A 5-byte Syringe jmp at 0x6CEF80 would overwrite the first byte of the
 * instruction at 0x6CEF84, where Antares writes its own jmp. Whichever DLL
 * injected second would corrupt the other. Do not hook 0x6CEF80.
 *
 * The function is a virtual reached only through the SuperWeaponTypeClass
 * vtable (no `call rel32` exists anywhere in gamemd), so we replace the vtable
 * slot instead and wrap it. Verified: vtable[0x7F40FC] == 0x6CEF80, and the
 * slot is unclaimed by all six frameworks in the registry. The wrapper calls
 * the original address directly, so Antares' 0x6CEF84 hook still runs inside it
 * and still gets to make its own decision first.
 *
 * (Calling the game address directly rather than through a YRpp qualified call
 * is deliberate — YRpp declares this virtual with R0, so a qualified non-virtual
 * call would silently no-op.)
 */
#include "Body.h"

#include <GeneralDefinitions.h>
#include <HouseClass.h>
#include <ObjectClass.h>
#include <SuperWeaponTypeClass.h>
#include <Utilities/Macro.h>

namespace
{
    // Vanilla's generic "this superweapon may not fire here" action. The engine
    // returns it at 0x6CEFCA; the enum spells it NoForceShield because the
    // ForceShield family owns the cursor, but it is used for every SW.
    constexpr auto VanillaDisallowed = static_cast<Action>(70);   // 0x46

    // Antares' private action codes for the superweapons it manages
    // (Antares src/Misc/Actions.h:9-10). Not in YRpp — they are Ares-lineage.
    constexpr auto AntaresAllowed    = static_cast<Action>(0x7F);
    constexpr auto AntaresDisallowed = static_cast<Action>(0x7E);
}

// =============================================================================
// Layer 2 — click veto
// =============================================================================
DEFINE_HOOK(0x4AC21C, DisplayClass_LeftMouseButtonUp_ConstraintVeto, 0x6)
{
    enum { Continue = 0, NoSuperWeapon = 0x4AC294 };

    GET(SuperWeaponTypeClass*, pSWType, EAX);
    GET_STACK(CellStruct, cell, 0x94);

    if (!pSWType)
        return Continue;

    if (SWTypeExt::AllowsCursorAt(pSWType, cell))
        return Continue;

    // One line per refused CLICK. This is the only place a player-initiated
    // refusal is observable: the veto below stops the SpecialPlace event ever
    // being queued, so the launch path never sees it.
    SWTypeExt::LogCursorDenial(pSWType, cell);

    // Fall into the same path the game uses when no superweapon resolved, so the
    // SpecialPlace event is never queued in the first place. Layer 1 would have
    // caught it anyway, but not queuing beats queuing-and-vetoing: it avoids a
    // pointless round trip through the network event list.
    return NoSuperWeapon;
}

// =============================================================================
// Layer 3 — cursor veto
// =============================================================================
namespace
{
    using GetActionFunc = Action(__fastcall*)(
        SuperWeaponTypeClass*, void*, CellStruct*, ObjectClass*);

    Action __fastcall SuperWeaponTypeClass_GetAction_Wrapper(
        SuperWeaponTypeClass* pThis, void*, CellStruct* pCell, ObjectClass* pTarget)
    {
        // Run the real function first — vanilla logic plus, if it is loaded,
        // Antares' 0x6CEF84 hook. Whatever they decide stands unless we deny.
        // This is what keeps the composition AND rather than a replacement.
        const auto original = reinterpret_cast<GetActionFunc>(0x6CEF80);
        const Action action = original(pThis, nullptr, pCell, pTarget);

        // They already said no, or this cell is not a superweapon target at all.
        if (action == VanillaDisallowed || action == AntaresDisallowed
            || action == Action::None || !pThis || !pCell)
        {
            return action;
        }

        if (SWTypeExt::AllowsCursorAt(pThis, *pCell))
            return action;

        // Deny in the caller's own vocabulary so each system's no-cursor is the
        // one that shows. This resolves the open question flagged in FINDINGS §6:
        // the right disallow code is not a guess, it is read off what the
        // original returned. An Antares-managed superweapon answers 0x7F, so we
        // answer 0x7E; anything else is vanilla, so we answer 0x46.
        return (action == AntaresAllowed) ? AntaresDisallowed : VanillaDisallowed;
    }
}

// Replace the vtable slot rather than patching code, so Antares' in-function
// hook at 0x6CEF84 survives untouched. vtable[0x7F40FC] == 0x6CEF80, verified.
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F40FC, SuperWeaponTypeClass_GetAction_Wrapper);

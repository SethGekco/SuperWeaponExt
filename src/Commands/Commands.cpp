/*
 * SuperWeaponExt — command registration.
 *
 * CommandClassCallback_Register @ 0x533058. Antares hooks 0x533058 (size 7);
 * Phobos AND AggressiveStance both hook 0x533066 (size 6) and co-load today, so
 * that address is proven shareable — Syringe chains all handlers that return 0.
 * We take 0x533066 as a third consumer, matching Phobos' own comment that this
 * runs after Ares-lineage registration.
 */
#include "FireNamedSW.h"

#include <Memory.h>
#include <Utilities/Macro.h>

#include <utility>   // integer_sequence

namespace
{
    // MakeCommand equivalent: the game owns the instance once registered, and
    // the array lives for the process lifetime. CommandClass::Array is
    // DEFINE_REFERENCE (a reference to the vector), hence `.AddItem`.
    template<typename T>
    void MakeCommand()
    {
        CommandClass::Array.AddItem(GameCreate<T>());
    }

    // Registering the pool is a compile-time unroll because each slot is a
    // distinct type (Index is a template parameter — that is what gives every
    // slot its own stable GetName()).
    template<int... Indices>
    void RegisterSlots(std::integer_sequence<int, Indices...>)
    {
        (MakeCommand<FireNamedSWCommandClass<Indices>>(), ...);
    }
}

DEFINE_HOOK(0x533066, SWExt_CommandClassCallback_Register, 0x6)
{
    RegisterSlots(std::make_integer_sequence<int, SWExtHotkeySlots>{});
    return 0;
}

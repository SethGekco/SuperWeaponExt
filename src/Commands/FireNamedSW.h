#pragma once
/*
 * SuperWeaponExt — dedicated per-superweapon hotkeys.
 *
 * NOT Phobos' FireTacticalSW. Those are POSITIONAL — "fire whatever sits in
 * SW-sidebar slot N" — and only work when the exclusive SW sidebar is enabled.
 * These bind a key to a NAMED superweapon, independent of the sidebar, of
 * whether the SW has a cameo at all, and of SuperWeaponSidebarKeysEnabled.
 *
 * WHY A FIXED POOL. Command identity is a compile-time C++ class whose
 * GetName() is the stable ASCII key that RA2MD.ini [Hotkey] binds against, and
 * registration runs during init (CommandClassCallback_Register @0x533058),
 * BEFORE rules.ini has been parsed into SuperWeaponTypeClass instances. So we
 * cannot emit one command per SWType at registration time. Instead we register
 * a fixed pool of slots, and each superweapon claims one:
 *
 *     [SOMESW]
 *     SWExt.HotkeyIndex=3      ; slot 3 belongs to this superweapon alone
 *
 * Every slot is still one superweapon's own dedicated key. The index is just
 * how the modder addresses it.
 *
 * (Whether commands can be registered AFTER rules parse — which would allow one
 * auto-named command per SWType — is unverified. See FINDINGS.md §1b.)
 */
#include <SuperWeaponExt.h>

#include <Ext/SWType/Body.h>

#include <CommandClass.h>
#include <GameStrings.h>
#include <HouseClass.h>
#include <StringTable.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>

#include <cstdio>    // _snprintf_s / _snwprintf_s
#include <iterator>  // std::size

// Phobos spells this in its own Commands.h, which we do not include (it drags in
// the whole Phobos command set). Same definition.
#ifndef CATEGORY_INTERFACE
#define CATEGORY_INTERFACE StringTable::LoadString(GameStrings::TXT_INTERFACE)
#endif

// SWExtHotkeySlots (the pool size) is declared in Ext/SWType/Body.h so the INI
// range check can see it without a circular include.

template<int Index>
class FireNamedSWCommandClass : public CommandClass
{
    virtual const char* GetName() const override;
    virtual const wchar_t* GetUIName() const override;
    virtual const wchar_t* GetUICategory() const override;
    virtual const wchar_t* GetUIDescription() const override;
    virtual void Execute(WWKey eInput) const override;
};

// The stable binding key. MUST NOT change once a mod ships: RA2MD.ini [Hotkey]
// stores this string, so renaming it silently unbinds every user's key.
template<int Index>
inline const char* FireNamedSWCommandClass<Index>::GetName() const
{
    _snprintf_s(SuperWeaponExtDLL::readBuffer, SuperWeaponExtDLL::readLength,
                "SWExtFireSW%d", Index + 1);
    return SuperWeaponExtDLL::readBuffer;
}

// Display name resolves at draw time from whichever superweapon claimed the
// slot, so the keyboard config reads "Fire Nuclear Missile" rather than
// "Fire Super Weapon 3". Falls back to the generic label when unclaimed.
template<int Index>
inline const wchar_t* FireNamedSWCommandClass<Index>::GetUIName() const
{
    if (auto const pType = SWTypeExt::FindByHotkeyIndex(Index))
    {
        if (const wchar_t* pUIName = pType->UIName)
        {
            if (*pUIName)
                return pUIName;
        }
    }

    const wchar_t* csf = StringTable::TryFetchString(
        "TXT_SWEXT_FIRE_SW_XX", L"Fire Super Weapon %d");
    _snwprintf_s(SuperWeaponExtDLL::wideBuffer, std::size(SuperWeaponExtDLL::wideBuffer),
                 csf, Index + 1);
    return SuperWeaponExtDLL::wideBuffer;
}

template<int Index>
inline const wchar_t* FireNamedSWCommandClass<Index>::GetUICategory() const
{
    return CATEGORY_INTERFACE;
}

template<int Index>
inline const wchar_t* FireNamedSWCommandClass<Index>::GetUIDescription() const
{
    const wchar_t* csf = StringTable::TryFetchString(
        "TXT_SWEXT_FIRE_SW_XX_DESC",
        L"Fires the super weapon that claimed SuperWeaponExt hotkey slot %d.");
    _snwprintf_s(SuperWeaponExtDLL::wideBuffer, std::size(SuperWeaponExtDLL::wideBuffer),
                 csf, Index + 1);
    return SuperWeaponExtDLL::wideBuffer;
}

template<int Index>
inline void FireNamedSWCommandClass<Index>::Execute(WWKey) const
{
    SWTypeExt::FireByHotkeyIndex(Index);
}

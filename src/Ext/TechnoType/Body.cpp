/*
 * SuperWeaponExt — TechnoTypeClass extension implementation + container hooks.
 *
 * Container lifecycle addresses are the ones Phobos uses for its own
 * TechnoTypeClass container. Phobos hooks these same sites for a DIFFERENT
 * container; every handler here returns 0, so Syringe chains them and the two
 * coexist. Confirmed against Phobos src/Ext/TechnoType/Body.cpp (develop).
 */
#include "Body.h"

#include <Utilities/Macro.h>

#include <cstdio>   // _snprintf_s

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

namespace
{
    // Read one veterancy-tiered range triplet: Key=, Key.Veteran=, Key.Elite=.
    // A missing key stays <0, which RangeSpec::Resolve reads as "inherit".
    void ReadRangeSpec(CCINIClass* pINI, const char* section, const char* key,
                       SWExt::RangeSpec& into)
    {
        char buffer[64] = {};

        into.Base = pINI->ReadInteger(section, key, into.Base);

        _snprintf_s(buffer, sizeof(buffer), "%s.Veteran", key);
        into.Veteran = pINI->ReadInteger(section, buffer, into.Veteran);

        _snprintf_s(buffer, sizeof(buffer), "%s.Elite", key);
        into.Elite = pINI->ReadInteger(section, buffer, into.Elite);
    }
}

void TechnoTypeExt::ExtData::LoadFromINIFile(CCINIClass* pINI)
{
    Extension<TechnoTypeClass>::LoadFromINIFile(pINI);

    const char* section = this->OwnerObject()->ID;

    ReadRangeSpec(pINI, section, "SWExt.InhibitorRange", this->InhibitorRange);
    ReadRangeSpec(pINI, section, "SWExt.DesignatorRange", this->DesignatorRange);
}

// Type data is re-parsed from the rules INI on every load, so there is nothing
// session-specific to serialize. Same reasoning as PrerequisiteExt.
template <typename T>
void TechnoTypeExt::ExtData::Serialize(T&) { }

void TechnoTypeExt::ExtData::LoadFromStream(PhobosStreamReader& stm)
{
    Extension<TechnoTypeClass>::LoadFromStream(stm);
    this->Serialize(stm);
}

void TechnoTypeExt::ExtData::SaveToStream(PhobosStreamWriter& stm)
{
    Extension<TechnoTypeClass>::SaveToStream(stm);
    this->Serialize(stm);
}

TechnoTypeExt::ExtContainer::ExtContainer()
    : Container("TechnoTypeClass")
{ }

TechnoTypeExt::ExtContainer::~ExtContainer() = default;

// =============================================================================
// Container lifecycle
//
// TechnoTypeClass has a shared base CTOR/DTOR, so ONE hook pair covers
// Infantry/Unit/Aircraft/BuildingType — no per-subclass hooks needed.
//
// Every address and register below is copied from Phobos
// src/Ext/TechnoType/Body.cpp:1977-2027 (develop @4747562), which maintains its
// own TechnoTypeClass container at these exact sites. Ours is a separate
// container and every handler returns 0, so Syringe chains the two.
// =============================================================================

DEFINE_HOOK(0x711835, TechnoTypeClass_CTOR, 0x5)
{
    GET(TechnoTypeClass*, pItem, ESI);

    TechnoTypeExt::ExtMap.TryAllocate(pItem);
    return 0;
}

DEFINE_HOOK(0x711AE0, TechnoTypeClass_DTOR, 0x5)
{
    GET(TechnoTypeClass*, pItem, ECX);

    TechnoTypeExt::ExtMap.Remove(pItem);
    return 0;
}

DEFINE_HOOK_AGAIN(0x716DC0, TechnoTypeClass_SaveLoad_Prefix, 0x5)
DEFINE_HOOK(0x7162F0, TechnoTypeClass_SaveLoad_Prefix, 0x6)
{
    GET_STACK(TechnoTypeClass*, pItem, 0x4);
    GET_STACK(IStream*, pStm, 0x8);

    TechnoTypeExt::ExtMap.PrepareStream(pItem, pStm);
    return 0;
}

DEFINE_HOOK(0x716DAC, TechnoTypeClass_Load_Suffix, 0xA)
{
    TechnoTypeExt::ExtMap.LoadStatic();
    return 0;
}

DEFINE_HOOK(0x717094, TechnoTypeClass_Save_Suffix, 0x5)
{
    TechnoTypeExt::ExtMap.SaveStatic();
    return 0;
}

DEFINE_HOOK(0x716123, TechnoTypeClass_LoadFromINI, 0x5)
{
    GET(TechnoTypeClass*, pItem, EBP);
    GET_STACK(CCINIClass*, pINI, 0x380);

    TechnoTypeExt::ExtMap.LoadFromINI(pItem, pINI);
    return 0;
}

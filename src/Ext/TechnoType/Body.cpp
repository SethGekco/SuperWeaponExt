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

#include <TechnoTypeClass.h>
#include <Utilities/Debug.h>

#include <cstdio>    // _snprintf_s
#include <cstring>   // _strcmpi
#include <string>
#include <vector>

TechnoTypeExt::ExtContainer TechnoTypeExt::ExtMap;

namespace
{
    // Split a comma list into trimmed, non-empty tokens.
    std::vector<std::string> SplitList(const char* buffer)
    {
        std::vector<std::string> out;
        std::string cur;
        for (const char* p = buffer; p && *p; ++p)
        {
            if (*p == ',')
            {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            }
            else if (*p != ' ' && *p != '\t')
            {
                cur += *p;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    void ReadStandingOrder(CCINIClass* pINI, const char* section,
                           SWExt::StandingOrder& into)
    {
        char buf[2048] = {};

        if (pINI->ReadString(section, "SWExt.StandingOrder", "", buf, sizeof(buf)) <= 0)
            return;

        using M = SWExt::StandingOrderMode;
        if (!_strcmpi(buf, "none") || !_strcmpi(buf, "no"))       into.Mode = M::None;
        else if (!_strcmpi(buf, "techno") || !_strcmpi(buf, "type")) into.Mode = M::Techno;
        else if (!_strcmpi(buf, "swimpact") || !_strcmpi(buf, "impact")) into.Mode = M::SWImpact;
        else
        {
            Debug::Log("[SuperWeaponExt] [%s] SWExt.StandingOrder='%s' is not "
                       "recognised (none/techno/swimpact); ignoring\n", section, buf);
            return;
        }

        if (pINI->ReadString(section, "SWExt.StandingOrder.Types", "", buf, sizeof(buf)) > 0)
        {
            for (auto const& tok : SplitList(buf))
            {
                if (auto const pType = TechnoTypeClass::Find(tok.c_str()))
                    into.TypeIndices.push_back(pType->GetArrayIndex());
                else
                    Debug::Log("[SuperWeaponExt] [%s] SWExt.StandingOrder.Types: "
                               "unknown TechnoType '%s'\n", section, tok.c_str());
            }
        }

        if (pINI->ReadString(section, "SWExt.StandingOrder.AffectsHouse", "", buf, sizeof(buf)) > 0)
        {
            using R = SWExt::Relation;
            if (!_strcmpi(buf, "owner") || !_strcmpi(buf, "self"))         into.Affects = R::Owner;
            else if (!_strcmpi(buf, "allies") || !_strcmpi(buf, "ally"))   into.Affects = R::Allies;
            else if (!_strcmpi(buf, "enemies") || !_strcmpi(buf, "enemy")) into.Affects = R::Enemies;
            else if (!_strcmpi(buf, "team"))                               into.Affects = R::Team;
            else if (!_strcmpi(buf, "notallies"))                          into.Affects = R::NotAllies;
            else if (!_strcmpi(buf, "notowner"))                           into.Affects = R::NotOwner;
            else if (!_strcmpi(buf, "all"))                                into.Affects = R::All;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.StandingOrder.AffectsHouse='%s' "
                           "is not recognised; using enemies\n", section, buf);
            }
        }

        into.Range    = pINI->ReadInteger(section, "SWExt.StandingOrder.Range", 0);
        into.Interval = pINI->ReadInteger(section, "SWExt.StandingOrder.Interval", 45);
        into.IdleOnly = pINI->ReadBool(section, "SWExt.StandingOrder.IdleOnly", true);

        if (into.Interval < 1)
            into.Interval = 1;

        if (into.Active())
        {
            Debug::Log("[SuperWeaponExt] [%s] standing order: mode %d, %u type(s), "
                       "range %d, every %d frames, idle-only %d\n",
                       section, static_cast<int>(into.Mode),
                       static_cast<unsigned>(into.TypeIndices.size()),
                       into.Range, into.Interval, into.IdleOnly ? 1 : 0);
        }
        else
        {
            Debug::Log("[SuperWeaponExt] [%s] SWExt.StandingOrder is set but inert "
                       "(techno mode needs SWExt.StandingOrder.Types)\n", section);
        }
    }

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

    // Leptons. 256 leptons = 1 cell; the engine default is 0x400 (4 cells).
    this->ParadropRadius = pINI->ReadInteger(section, "SWExt.ParadropRadius", -1);

    ReadStandingOrder(pINI, section, this->Order);
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

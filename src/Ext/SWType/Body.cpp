/*
 * SuperWeaponExt — SuperWeaponTypeClass extension implementation.
 *
 * Container lifecycle addresses copied from Phobos src/Ext/SWType/Body.cpp:346-392
 * (develop @4747562). Phobos maintains its own SWType container at these sites;
 * ours is separate and every handler returns 0, so Syringe chains them.
 */
#include "Body.h"

#include <Ext/TechnoType/Body.h>

#include <BuildingClass.h>
#include <CellClass.h>
#include <DisplayClass.h>
#include <EventClass.h>
#include <GeneralDefinitions.h>
#include <HouseClass.h>
#include <SuperClass.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <WWMouseClass.h>
#include <Helpers/Cast.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <cstdio>    // _snprintf_s
#include <cstdlib>   // atoi
#include <cstring>   // _strcmpi
#include <string>
#include <vector>

SWTypeExt::ExtContainer SWTypeExt::ExtMap;

// =============================================================================
// Parsing
// =============================================================================
namespace
{
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

    std::vector<std::string> ReadList(CCINIClass* pINI, const char* section, const char* key)
    {
        char buffer[2048] = {};
        if (pINI->ReadString(section, key, "", buffer, sizeof(buffer)) <= 0)
            return {};
        return SplitList(buffer);
    }

    // Parse an AffectsHouse mask. Accepts a comma list so
    // "owner,enemies" and the shorthand "notallies" both work — the token names
    // match Antares' SuperWeaponAffectedHouse spellings so modders do not have
    // to learn a second vocabulary.
    bool ParseRelation(CCINIClass* pINI, const char* section, const char* key,
                       SWExt::Relation& into)
    {
        char buffer[128] = {};
        if (pINI->ReadString(section, key, "", buffer, sizeof(buffer)) <= 0)
            return false;

        SWExt::Relation result = SWExt::Relation::None;
        bool any = false;

        for (auto const& tok : SplitList(buffer))
        {
            const char* t = tok.c_str();
            SWExt::Relation bit = SWExt::Relation::None;

            if (!_strcmpi(t, "owner") || !_strcmpi(t, "self"))          bit = SWExt::Relation::Owner;
            else if (!_strcmpi(t, "allies") || !_strcmpi(t, "ally"))    bit = SWExt::Relation::Allies;
            else if (!_strcmpi(t, "enemies") || !_strcmpi(t, "enemy"))  bit = SWExt::Relation::Enemies;
            else if (!_strcmpi(t, "team"))                              bit = SWExt::Relation::Team;
            else if (!_strcmpi(t, "notallies"))                         bit = SWExt::Relation::NotAllies;
            else if (!_strcmpi(t, "notowner"))                          bit = SWExt::Relation::NotOwner;
            else if (!_strcmpi(t, "all"))                               bit = SWExt::Relation::All;
            else if (!_strcmpi(t, "none"))                              bit = SWExt::Relation::None;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s]%s: unknown house relation '%s' "
                           "(expected owner/allies/enemies/team/notallies/notowner/all)\n",
                           section, key, t);
                continue;
            }

            result = result | bit;
            any = true;
        }

        if (any)
            into = result;
        return any;
    }

    // Read one role's block: the type list, the parallel range overrides, the
    // wildcard, the relation mask and the power requirement.
    void ReadRule(CCINIClass* pINI, const char* section, const char* prefix,
                  SWExt::Relation defaultRelation, bool defaultRequirePower,
                  SWExt::Rule& into)
    {
        char key[128] = {};

        // Types
        _snprintf_s(key, sizeof(key), "%s", prefix);
        for (auto const& tok : ReadList(pINI, section, key))
        {
            if (auto const pType = TechnoTypeClass::Find(tok.c_str()))
                into.TypeIndices.push_back(pType->GetArrayIndex());
            else
                Debug::Log("[SuperWeaponExt] [%s]%s: unknown TechnoType '%s'\n",
                           section, key, tok.c_str());
        }

        // Per-SW range overrides, positionally matched to the type list above.
        // Shorter list = the tail falls back to the TechnoType range.
        _snprintf_s(key, sizeof(key), "%s.Ranges", prefix);
        for (auto const& tok : ReadList(pINI, section, key))
            into.RangesByIndex.push_back(std::atoi(tok.c_str()));

        if (into.RangesByIndex.size() > into.TypeIndices.size())
        {
            Debug::Log("[SuperWeaponExt] [%s]%s: %u ranges given for %u types; "
                       "the extras are ignored\n", section, key,
                       static_cast<unsigned>(into.RangesByIndex.size()),
                       static_cast<unsigned>(into.TypeIndices.size()));
        }

        _snprintf_s(key, sizeof(key), "%s.Any", prefix);
        into.Any = pINI->ReadBool(section, key, false);

        into.Affects = defaultRelation;
        _snprintf_s(key, sizeof(key), "%s.AffectsHouse", prefix);
        ParseRelation(pINI, section, key, into.Affects);

        _snprintf_s(key, sizeof(key), "%s.RequirePower", prefix);
        into.RequirePower = pINI->ReadBool(section, key, defaultRequirePower);
    }
}

void SWTypeExt::ExtData::LoadFromINIFile(CCINIClass* pINI)
{
    Extension<SuperWeaponTypeClass>::LoadFromINIFile(pINI);

    const char* section = this->OwnerObject()->ID;

    this->Inhibitors = SWExt::Rule{};
    this->Designators = SWExt::Rule{};

    // Defaults reproduce Antares' hardcoded behaviour, so a mod that only adds
    // the type lists behaves exactly as it would under SW.Inhibitors /
    // SW.Designators. Everything beyond that is opt-in.
    //   - inhibitors: enemies only, and buildings must be powered
    //   - designators: owner only, no power check
    ReadRule(pINI, section, "SWExt.Inhibitors",
             SWExt::Relation::Enemies, /*requirePower=*/true, this->Inhibitors);
    ReadRule(pINI, section, "SWExt.Designators",
             SWExt::Relation::Owner, /*requirePower=*/false, this->Designators);

    // Dedicated hotkey slot. -1 (the default) means this SW claims none.
    this->HotkeyIndex = pINI->ReadInteger(section, "SWExt.HotkeyIndex", -1);

    if (this->HotkeyIndex >= SWExtHotkeySlots)
    {
        Debug::Log("[SuperWeaponExt] [%s] SWExt.HotkeyIndex=%d is out of range "
                   "(0..%d); ignoring\n", section, this->HotkeyIndex,
                   SWExtHotkeySlots - 1);
        this->HotkeyIndex = -1;
    }

    // Tri-state: absent leaves -1 (auto). See the header for why auto is
    // unreliable under Antares. Probed with ReadString first because ReadBool
    // cannot distinguish "absent" from "explicitly no".
    {
        char probe[32] = {};
        if (pINI->ReadString(section, "SWExt.Hotkey.FireInstantly", "", probe, sizeof(probe)) > 0)
        {
            this->HotkeyFireInstantly =
                pINI->ReadBool(section, "SWExt.Hotkey.FireInstantly", false) ? 1 : 0;
        }
    }

    // Where an instant hotkey launch aims.
    {
        char mode[32] = {};
        if (pINI->ReadString(section, "SWExt.Hotkey.Target", "", mode, sizeof(mode)) > 0)
        {
            using M = SWTypeExt::ExtData::HotkeyTargetMode;
            if (!_strcmpi(mode, "mouse") || !_strcmpi(mode, "cursor")) this->HotkeyTarget = M::Mouse;
            else if (!_strcmpi(mode, "screen") || !_strcmpi(mode, "view")) this->HotkeyTarget = M::Screen;
            else if (!_strcmpi(mode, "base"))                             this->HotkeyTarget = M::Base;
            else if (!_strcmpi(mode, "cell"))                             this->HotkeyTarget = M::Cell;
            else if (!_strcmpi(mode, "none"))                             this->HotkeyTarget = M::None;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.Hotkey.Target='%s' is not "
                           "recognised (mouse/screen/base/cell/none); using mouse\n",
                           section, mode);
            }
        }

        char cellBuf[64] = {};
        if (pINI->ReadString(section, "SWExt.Hotkey.TargetCell", "", cellBuf, sizeof(cellBuf)) > 0)
        {
            int x = 0, y = 0;
            if (sscanf_s(cellBuf, "%d,%d", &x, &y) == 2)
            {
                this->HotkeyTargetCell.X = static_cast<short>(x);
                this->HotkeyTargetCell.Y = static_cast<short>(y);
            }
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.Hotkey.TargetCell='%s' is not "
                           "an X,Y pair; ignoring\n", section, cellBuf);
            }
        }
    }

    if (this->HotkeyIndex >= 0)
    {
        using M = SWTypeExt::ExtData::HotkeyTargetMode;
        const char* target =
              this->HotkeyTarget == M::Mouse  ? "mouse"
            : this->HotkeyTarget == M::Screen ? "screen centre"
            : this->HotkeyTarget == M::Base   ? "base centre"
            : this->HotkeyTarget == M::Cell   ? "fixed cell"
                                              : "none (0,0)";

        Debug::Log("[SuperWeaponExt] [%s] claims hotkey slot %d (command "
                   "SWExtFireSW%d); on press it will %s; instant target = %s\n",
                   section, this->HotkeyIndex, this->HotkeyIndex + 1,
                   this->HotkeyFireInstantly == 1 ? "FIRE immediately"
                 : this->HotkeyFireInstantly == 0 ? "ARM the cursor"
                 : "auto-decide from Action (unreliable under Antares — set "
                   "SWExt.Hotkey.FireInstantly explicitly)",
                   target);
    }

    if (this->IsConfigured())
    {
        Debug::Log("[SuperWeaponExt] [%s] %u inhibitor type(s)%s, "
                   "%u designator type(s)%s\n", section,
                   static_cast<unsigned>(this->Inhibitors.TypeIndices.size()),
                   this->Inhibitors.Any ? " +Any" : "",
                   static_cast<unsigned>(this->Designators.TypeIndices.size()),
                   this->Designators.Any ? " +Any" : "");
    }
}

// =============================================================================
// The decision
// =============================================================================
bool SWTypeExt::ExtData::AllowsFireAt(HouseClass* pFirer, const CellStruct& cell) const
{
    if (!this->IsConfigured() || !pFirer)
        return true;

    std::vector<SWExt::Source> sources;
    sources.reserve(64);

    // TechnoClass::Array is DEFINE_REFERENCE (a reference to the vector, not a
    // pointer to it) — hence `.Count`, not `->Count`.
    for (int i = 0; i < TechnoClass::Array.Count; ++i)
    {
        TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
        if (!pTechno)
            continue;

        TechnoTypeClass* const pType = pTechno->GetTechnoType();
        if (!pType)
            continue;

        const int typeIndex = pType->GetArrayIndex();

        // Skip anything neither rule could possibly care about. This is the
        // cheap filter that keeps the per-frame cost near Antares' own.
        if (!this->Inhibitors.CoversType(typeIndex) && !this->Designators.CoversType(typeIndex))
            continue;

        SWExt::Source src;
        src.TypeIndex = typeIndex;
        src.Active = pTechno->IsAlive && pTechno->Health > 0
                  && !pTechno->InLimbo && !pTechno->Deactivated;

        if (!src.Active)
            continue;

        // Relation of this techno's owner TO the firing house. Mind control does
        // not launder an enemy into a friend: Owner is the *current* controller,
        // which is the same call Antares makes.
        HouseClass* const pOwner = pTechno->Owner;
        if (pOwner == pFirer)
            src.Rel = SWExt::Relation::Owner;
        else if (pFirer->IsAlliedWith(pOwner))
            src.Rel = SWExt::Relation::Allies;
        else
            src.Rel = SWExt::Relation::Enemies;

        src.Vet = pTechno->Veterancy.IsElite()   ? SWExt::Rank::Elite
                : pTechno->Veterancy.IsVeteran() ? SWExt::Rank::Veteran
                                                 : SWExt::Rank::Rookie;

        // Power only means anything for buildings; everything else counts as
        // powered so RequirePower does not silently disable mobile inhibitors.
        if (auto const pBld = abstract_cast<BuildingClass*>(pTechno))
            src.Powered = pBld->IsPowerOnline();
        else
            src.Powered = true;

        const auto center = CellClass::Coord2Cell(pTechno->GetCoords());
        src.CellX = center.X;
        src.CellY = center.Y;

        // Veterancy-resolved TechnoType range, used when the SW gives no override.
        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        const bool isDesignator = this->Designators.CoversType(typeIndex);
        auto const& spec = isDesignator ? pTypeExt->DesignatorRange
                                        : pTypeExt->InhibitorRange;
        src.FallbackRange = spec.Resolve(src.Vet, pType->Sight);

        sources.push_back(src);
    }

    return SWExt::Allows(this->Inhibitors, this->Designators,
                         sources, cell.X, cell.Y);
}

bool SWTypeExt::AllowsCursorAt(SuperWeaponTypeClass* pType, const CellStruct& cell)
{
    if (!pType)
        return true;

    auto const pExt = SWTypeExt::ExtMap.Find(pType);

    // Not ours to police, or no local player to evaluate against (observer,
    // loading screen) — say nothing and let the incumbent's verdict stand.
    if (!pExt || !pExt->IsConfigured())
        return true;

    // DEFINE_REFERENCE(HouseClass*, CurrentPlayer, 0xA83D4C) — a reference to
    // the pointer, so no call parentheses.
    auto const pPlayer = HouseClass::CurrentPlayer;
    if (!pPlayer)
        return true;

    return pExt->AllowsFireAt(pPlayer, cell);
}

// =============================================================================
// Dedicated per-superweapon hotkeys
// =============================================================================
CellStruct SWTypeExt::ExtData::ResolveHotkeyCell(HouseClass* pFirer) const
{
    using M = SWTypeExt::ExtData::HotkeyTargetMode;

    switch (this->HotkeyTarget)
    {
    case M::Cell:
        return this->HotkeyTargetCell;

    case M::None:
        return CellStruct::Empty;

    case M::Base:
        // BaseCenter, falling back to the spawn cell if the house has no base yet.
        return pFirer ? pFirer->GetBaseCenter() : CellStruct::Empty;

    case M::Screen:
        if (auto const pTac = TacticalClass::Instance)
        {
            // The tactical view rectangle; centre of it in client coordinates.
            auto const& view = *reinterpret_cast<RectangleStruct*>(0xB0CE28);
            const Point2D centre{ view.Width / 2, view.Height / 2 };
            return CellClass::Coord2Cell(pTac->ClientToCoords(centre));
        }
        return CellStruct::Empty;

    case M::Mouse:
    default:
        if (auto const pTac = TacticalClass::Instance)
        {
            if (auto const pMouse = WWMouseClass::Instance)
            {
                Point2D pt{};
                pMouse->GetCoords(&pt);
                // Mouse coords are screen-absolute; ClientToCoords wants them
                // relative to the tactical view's origin.
                auto const& view = *reinterpret_cast<RectangleStruct*>(0xB0CE28);
                pt.X -= view.X;
                pt.Y -= view.Y;
                return CellClass::Coord2Cell(pTac->ClientToCoords(pt));
            }
        }
        return CellStruct::Empty;
    }
}

SuperWeaponTypeClass* SWTypeExt::FindByHotkeyIndex(int index)
{
    if (index < 0)
        return nullptr;

    for (int i = 0; i < SuperWeaponTypeClass::Array.Count; ++i)
    {
        SuperWeaponTypeClass* const pType = SuperWeaponTypeClass::Array.GetItem(i);
        if (!pType)
            continue;

        auto const pExt = SWTypeExt::ExtMap.Find(pType);
        if (pExt && pExt->HotkeyIndex == index)
            return pType;   // first claimant wins
    }

    return nullptr;
}

void SWTypeExt::FireByHotkeyIndex(int index)
{
    SuperWeaponTypeClass* const pType = SWTypeExt::FindByHotkeyIndex(index);
    if (!pType)
        return;   // nothing claimed this slot

    HouseClass* const pPlayer = HouseClass::CurrentPlayer;
    if (!pPlayer)
        return;

    SuperClass* const pSuper = pPlayer->Supers.GetItemOrDefault(pType->ArrayIndex);
    if (!pSuper)
        return;   // this house does not own the superweapon

    if (!pSuper->CanFire())
        return;   // still charging, on hold, or otherwise unavailable

    auto const pExt = SWTypeExt::ExtMap.Find(pType);

    // -1 auto / 0 arm / 1 fire. Auto falls back to the Action test, which is
    // correct without Antares but never true with it — Antares forces
    // Action = SuperWeaponAllowed on every SW it handles. Verified in-game.
    const bool fireInstantly =
        (pExt && pExt->HotkeyFireInstantly >= 0)
            ? (pExt->HotkeyFireInstantly == 1)
            : (pType->Action == Action::None);

    if (fireInstantly)
    {
        // No target needed — fire immediately. This is the invisible-superweapon
        // case: no cameo, no cursor, just a key. Queueing the event (rather than
        // calling Fire_SW directly) is what makes it network-safe: every client
        // executes it on the same frame. It lands in HouseClass::Fire_SW, so the
        // Layer 1 inhibitor/designator veto applies with no extra work.
        //
        // Where it lands comes from SWExt.Hotkey.Target. Resolving mouse/view
        // state here is safe: the resolved cell travels inside the event, so
        // every client executes the same one.
        //
        // (If the superweapon sets Antares' SW.UseAITargeting=yes, Antares'
        // SpecialPlace handler ignores this cell entirely and runs its own AI
        // target picker instead — see Antares Hooks.Targeting.cpp:660.)
        const CellStruct cell = pExt ? pExt->ResolveHotkeyCell(pPlayer)
                                     : CellStruct::Empty;

        EventClass::OutList.Add(EventClass(
            pPlayer->ArrayIndex, EventType::SpecialPlace,
            pType->ArrayIndex, cell));
        return;
    }

    // Targeted superweapon: arm it, exactly as clicking its sidebar cameo does.
    // The next left click fires it, and passes through Layers 2 and 3 on the way.
    DisplayClass::Instance.CurrentBuilding = nullptr;
    DisplayClass::Instance.CurrentBuildingType = nullptr;
    DisplayClass::Instance.CurrentBuildingOwnerArrayIndex = -1;
    DisplayClass::Instance.CurrentSWTypeIndex = pType->ArrayIndex;
}

// Rules are static type data re-parsed from the rules INI on every load, so
// there is nothing session-specific to serialize.
template <typename T>
void SWTypeExt::ExtData::Serialize(T&) { }

void SWTypeExt::ExtData::LoadFromStream(PhobosStreamReader& stm)
{
    Extension<SuperWeaponTypeClass>::LoadFromStream(stm);
    this->Serialize(stm);
}

void SWTypeExt::ExtData::SaveToStream(PhobosStreamWriter& stm)
{
    Extension<SuperWeaponTypeClass>::SaveToStream(stm);
    this->Serialize(stm);
}

SWTypeExt::ExtContainer::ExtContainer()
    : Container("SuperWeaponTypeClass")
{ }

SWTypeExt::ExtContainer::~ExtContainer() = default;

// =============================================================================
// Container lifecycle — addresses from Phobos src/Ext/SWType/Body.cpp
// =============================================================================

DEFINE_HOOK(0x6CE6F6, SuperWeaponTypeClass_CTOR, 0x5)
{
    GET(SuperWeaponTypeClass*, pItem, EAX);

    SWTypeExt::ExtMap.TryAllocate(pItem);
    return 0;
}

DEFINE_HOOK(0x6CEFE0, SuperWeaponTypeClass_SDDTOR, 0x8)
{
    GET(SuperWeaponTypeClass*, pItem, ECX);

    SWTypeExt::ExtMap.Remove(pItem);
    return 0;
}

DEFINE_HOOK_AGAIN(0x6CE8D0, SuperWeaponTypeClass_SaveLoad_Prefix, 0x8)
DEFINE_HOOK(0x6CE800, SuperWeaponTypeClass_SaveLoad_Prefix, 0xA)
{
    GET_STACK(SuperWeaponTypeClass*, pItem, 0x4);
    GET_STACK(IStream*, pStm, 0x8);

    SWTypeExt::ExtMap.PrepareStream(pItem, pStm);
    return 0;
}

DEFINE_HOOK(0x6CE8BE, SuperWeaponTypeClass_Load_Suffix, 0x7)
{
    SWTypeExt::ExtMap.LoadStatic();
    return 0;
}

DEFINE_HOOK(0x6CE8EA, SuperWeaponTypeClass_Save_Suffix, 0x3)
{
    SWTypeExt::ExtMap.SaveStatic();
    return 0;
}

DEFINE_HOOK(0x6CEE43, SuperWeaponTypeClass_LoadFromINI, 0xA)
{
    GET(SuperWeaponTypeClass*, pItem, EBP);
    GET_STACK(CCINIClass*, pINI, 0x3FC);

    SWTypeExt::ExtMap.LoadFromINI(pItem, pINI);
    return 0;
}

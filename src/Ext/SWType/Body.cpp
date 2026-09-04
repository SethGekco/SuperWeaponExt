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
#include <ObjectClass.h>
#include <Surface.h>
#include <SuperClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Fundamentals.h>   // Unsorted::CurrentFrame (0xA8ED84)
#include <Unsorted.h>
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

        // --- growth over match time ---
        // Read as a double so fractional rates work (`Growth=0.25`), then scale
        // to an integer immediately. Parsing happens once per rules load from
        // identical INI text on every client, so the result is identical too —
        // and no float survives into the per-frame maths.
        _snprintf_s(key, sizeof(key), "%s.Growth", prefix);
        {
            const double perMinute = pINI->ReadDouble(section, key, 0.0);
            into.Growth.MilliPerMinute =
                static_cast<int>(perMinute * SWExt::GrowthScale
                                 + (perMinute >= 0.0 ? 0.5 : -0.5));
        }

        _snprintf_s(key, sizeof(key), "%s.Growth.Min", prefix);
        into.Growth.Min = pINI->ReadInteger(section, key, -1);

        _snprintf_s(key, sizeof(key), "%s.Growth.Max", prefix);
        into.Growth.Max = pINI->ReadInteger(section, key, -1);

        // --- ratio vs. counted technos ---
        _snprintf_s(key, sizeof(key), "%s.Ratio", prefix);
        for (auto const& tok : ReadList(pINI, section, key))
        {
            if (auto const pType = TechnoTypeClass::Find(tok.c_str()))
                into.Ratio.TypeIndices.push_back(pType->GetArrayIndex());
            else
                Debug::Log("[SuperWeaponExt] [%s]%s: unknown TechnoType '%s'\n",
                           section, key, tok.c_str());
        }

        into.Ratio.Affects = SWExt::Relation::All;
        _snprintf_s(key, sizeof(key), "%s.Ratio.AffectsHouse", prefix);
        ParseRelation(pINI, section, key, into.Ratio.Affects);

        _snprintf_s(key, sizeof(key), "%s.Ratio.Range", prefix);
        into.Ratio.Range = pINI->ReadInteger(section, key, 0);

        _snprintf_s(key, sizeof(key), "%s.Ratio.PerUnit", prefix);
        into.Ratio.PerUnit = pINI->ReadInteger(section, key, 0);

        _snprintf_s(key, sizeof(key), "%s.Ratio.Max", prefix);
        into.Ratio.Max = pINI->ReadInteger(section, key, 0);

        if (!into.Ratio.TypeIndices.empty() && into.Ratio.PerUnit == 0)
        {
            Debug::Log("[SuperWeaponExt] [%s] %s.Ratio lists types but %s.Ratio.PerUnit "
                       "is 0, so the ratio does nothing\n", section, prefix, prefix);
        }
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

    this->KeepSelectedAfterFire =
        pINI->ReadBool(section, "SWExt.KeepSelectedAfterFire", false);

    // --- owned paradrop ---
    this->ParaDrop = ParaDropConfig{};
    this->ParaDrop.Enabled = pINI->ReadBool(section, "SWExt.ParaDrop", false);

    if (this->ParaDrop.Enabled)
    {
        char buf[2048] = {};

        if (pINI->ReadString(section, "SWExt.ParaDrop.Aircraft", "", buf, sizeof(buf)) > 0)
        {
            this->ParaDrop.Aircraft = AircraftTypeClass::Find(buf);
            if (!this->ParaDrop.Aircraft)
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop.Aircraft: unknown "
                           "AircraftType '%s'\n", section, buf);
            }
        }

        for (auto const& tok : ReadList(pINI, section, "SWExt.ParaDrop.Types"))
        {
            if (auto const pT = TechnoTypeClass::Find(tok.c_str()))
                this->ParaDrop.Types.push_back(pT);
            else
                Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop.Types: unknown "
                           "TechnoType '%s'\n", section, tok.c_str());
        }

        for (auto const& tok : ReadList(pINI, section, "SWExt.ParaDrop.Nums"))
            this->ParaDrop.Nums.push_back(std::atoi(tok.c_str()));

        // Types and Nums are positional partners; a mismatch would silently drop
        // the wrong contents, so pad or trim and say so.
        if (this->ParaDrop.Nums.size() != this->ParaDrop.Types.size())
        {
            Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop: %u Types but %u Nums; "
                       "padding with 1\n", section,
                       static_cast<unsigned>(this->ParaDrop.Types.size()),
                       static_cast<unsigned>(this->ParaDrop.Nums.size()));
            this->ParaDrop.Nums.resize(this->ParaDrop.Types.size(), 1);
        }

        this->ParaDrop.Planes  = pINI->ReadInteger(section, "SWExt.ParaDrop.Planes", 1);
        this->ParaDrop.Spacing = pINI->ReadInteger(section, "SWExt.ParaDrop.Spacing", 4);

        if (pINI->ReadString(section, "SWExt.ParaDrop.Formation", "", buf, sizeof(buf)) > 0)
        {
            if (!_strcmpi(buf, "line"))        this->ParaDrop.Kind = SWExt::Formation::Line;
            else if (!_strcmpi(buf, "column")) this->ParaDrop.Kind = SWExt::Formation::Column;
            else if (!_strcmpi(buf, "wedge"))  this->ParaDrop.Kind = SWExt::Formation::Wedge;
            else if (!_strcmpi(buf, "box"))    this->ParaDrop.Kind = SWExt::Formation::Box;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop.Formation='%s' is not "
                           "recognised (line/column/wedge/box); using line\n", section, buf);
            }
        }

        // Accepts yes/no as well as the explicit modes, because "should the
        // formation follow the direction it comes from?" is naturally a boolean
        // question even though there are two distinct ways to say yes.
        if (pINI->ReadString(section, "SWExt.ParaDrop.Formation.Align", "", buf, sizeof(buf)) > 0)
        {
            using A = SWExt::FormationAlign;
            if (!_strcmpi(buf, "screen") || !_strcmpi(buf, "yes") || !_strcmpi(buf, "true"))
                this->ParaDrop.Align = A::Screen;
            else if (!_strcmpi(buf, "cell"))
                this->ParaDrop.Align = A::Cell;
            else if (!_strcmpi(buf, "map") || !_strcmpi(buf, "no") || !_strcmpi(buf, "false"))
                this->ParaDrop.Align = A::Map;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop.Formation.Align='%s' is "
                           "not recognised (screen/cell/map, or yes/no); using screen\n",
                           section, buf);
            }
        }

        if (pINI->ReadString(section, "SWExt.ParaDrop.Origin", "", buf, sizeof(buf)) > 0)
        {
            if (!_strcmpi(buf, "owner"))        this->ParaDrop.Origin = ParaDropOrigin::Owner;
            else if (!_strcmpi(buf, "nearest")) this->ParaDrop.Origin = ParaDropOrigin::Nearest;
            else if (!_strcmpi(buf, "north"))   this->ParaDrop.Origin = ParaDropOrigin::North;
            else if (!_strcmpi(buf, "east"))    this->ParaDrop.Origin = ParaDropOrigin::East;
            else if (!_strcmpi(buf, "south"))   this->ParaDrop.Origin = ParaDropOrigin::South;
            else if (!_strcmpi(buf, "west"))    this->ParaDrop.Origin = ParaDropOrigin::West;
            else
            {
                Debug::Log("[SuperWeaponExt] [%s] SWExt.ParaDrop.Origin='%s' is not "
                           "recognised (owner/nearest/north/east/south/west); "
                           "using owner\n", section, buf);
            }
        }

        // Explicit per-plane offsets as "X,Y|X,Y|..." — these replace the
        // generated formation entirely.
        if (pINI->ReadString(section, "SWExt.ParaDrop.Offsets", "", buf, sizeof(buf)) > 0)
        {
            const char* p = buf;
            while (*p)
            {
                int x = 0, y = 0;
                if (sscanf_s(p, "%d,%d", &x, &y) == 2)
                    this->ParaDrop.Offsets.push_back(SWExt::Offset{ x, y });

                const char* next = strchr(p, '|');
                if (!next)
                    break;
                p = next + 1;
            }
        }

        for (auto const& tok : ReadList(pINI, section, "SWExt.ParaDrop.Delays"))
            this->ParaDrop.Delays.push_back(std::atoi(tok.c_str()));

        Debug::Log("[SuperWeaponExt] [%s] owns its paradrop: %d plane(s), origin %d, "
                   "%u explicit offset(s), %u delay(s)\n", section,
                   this->ParaDrop.Planes, static_cast<int>(this->ParaDrop.Origin),
                   static_cast<unsigned>(this->ParaDrop.Offsets.size()),
                   static_cast<unsigned>(this->ParaDrop.Delays.size()));
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
        Debug::Log("[SuperWeaponExt] [%s] inhibitors: growth %d/1000 per min "
                   "(min %d max %d), ratio %u type(s) x%d within %d\n",
                   section,
                   this->Inhibitors.Growth.MilliPerMinute,
                   this->Inhibitors.Growth.Min, this->Inhibitors.Growth.Max,
                   static_cast<unsigned>(this->Inhibitors.Ratio.TypeIndices.size()),
                   this->Inhibitors.Ratio.PerUnit, this->Inhibitors.Ratio.Range);

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
// Reduce the live techno array to the plain-data view the evaluator needs.
// Shared by the verdict and the denial diagnostic so they can never disagree
// about what the world looked like.
void SWTypeExt::ExtData::GatherSources(HouseClass* pFirer,
                                       std::vector<SWExt::Source>& sources,
                                       SWExt::EvalContext& ctx) const
{
    if (!pFirer)
        return;

    sources.reserve(64);
    // The SYNCED frame counter. Growth must never key off wall-clock or render
    // time, or clients disagree about the radius and therefore the verdict.
    ctx.Frames = Unsorted::CurrentFrame;

    const bool wantRatio = this->Inhibitors.Ratio.Active()
                        || this->Designators.Ratio.Active();
    if (wantRatio)
        ctx.RatioSources.reserve(64);

    // ONE pass collects both kinds. Ratio counting deliberately does NOT rescan
    // the array per candidate — that is what would make the cursor path O(n²),
    // and this whole function already runs once per cursor frame.
    //
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

        const bool isSource = this->Inhibitors.CoversType(typeIndex)
                           || this->Designators.CoversType(typeIndex);
        const bool isRatio  = wantRatio
                           && (this->Inhibitors.Ratio.CountsType(typeIndex)
                            || this->Designators.Ratio.CountsType(typeIndex));

        // Cheap filter: skip anything no rule could possibly care about.
        if (!isSource && !isRatio)
            continue;

        if (!(pTechno->IsAlive && pTechno->Health > 0
              && !pTechno->InLimbo && !pTechno->Deactivated))
        {
            continue;
        }

        // Relation of this techno's owner TO the firing house. Mind control does
        // not launder an enemy into a friend: Owner is the *current* controller,
        // which is the same call Antares makes.
        HouseClass* const pOwner = pTechno->Owner;
        SWExt::Relation rel;
        if (pOwner == pFirer)
            rel = SWExt::Relation::Owner;
        else if (pFirer->IsAlliedWith(pOwner))
            rel = SWExt::Relation::Allies;
        else
            rel = SWExt::Relation::Enemies;

        const auto center = CellClass::Coord2Cell(pTechno->GetCoords());

        if (isRatio)
        {
            SWExt::RatioSource rs;
            rs.TypeIndex = typeIndex;
            rs.Rel       = rel;
            rs.CellX     = center.X;
            rs.CellY     = center.Y;
            rs.Active    = true;
            ctx.RatioSources.push_back(rs);
        }

        if (!isSource)
            continue;

        SWExt::Source src;
        src.TypeIndex = typeIndex;
        src.Active    = true;
        src.Rel       = rel;
        src.CellX     = center.X;
        src.CellY     = center.Y;

        src.Vet = pTechno->Veterancy.IsElite()   ? SWExt::Rank::Elite
                : pTechno->Veterancy.IsVeteran() ? SWExt::Rank::Veteran
                                                 : SWExt::Rank::Rookie;

        // Power only means anything for buildings; everything else counts as
        // powered so RequirePower does not silently disable mobile inhibitors.
        if (auto const pBld = abstract_cast<BuildingClass*>(pTechno))
            src.Powered = pBld->IsPowerOnline();
        else
            src.Powered = true;

        // Veterancy-resolved TechnoType range, used when the SW gives no override.
        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        const bool isDesignator = this->Designators.CoversType(typeIndex);
        auto const& spec = isDesignator ? pTypeExt->DesignatorRange
                                        : pTypeExt->InhibitorRange;
        src.FallbackRange = spec.Resolve(src.Vet, pType->Sight);

        sources.push_back(src);
    }

}

bool SWTypeExt::ExtData::AllowsFireAt(HouseClass* pFirer, const CellStruct& cell) const
{
    if (!this->IsConfigured() || !pFirer)
        return true;

    std::vector<SWExt::Source> sources;
    SWExt::EvalContext ctx;
    this->GatherSources(pFirer, sources, ctx);

    return SWExt::Allows(this->Inhibitors, this->Designators,
                         sources, cell.X, cell.Y, ctx);
}

void SWTypeExt::ExtData::LogDenial(HouseClass* pFirer, const CellStruct& cell) const
{
    // Layer 2 fires once per refused click, so it always prints. Seeding the
    // signature with a value LogDenialSig never produces defeats the throttle.
    int always = 0x7FFFFFFF;
    this->LogDenialSig(pFirer, cell, always);
}

// Explain the refusal, printing only when the explanation is NEW.
//
// `sig` is in/out: pass the last signature seen and it is updated to the current
// one. Nothing is printed when the signature is unchanged, which is what lets a
// caller on the cursor path (every mouse move) stay quiet until the answer
// actually moves — e.g. until a tank parks next to the power plant and the
// radius jumps.
void SWTypeExt::ExtData::LogDenialSig(HouseClass* pFirer, const CellStruct& cell,
                                      int& sig) const
{
    if (!pFirer || !this->Inhibitors.Active())
        return;

    // Rebuild the same view the evaluator saw. Cheap: this runs once, on a shot
    // that was actually refused.
    std::vector<SWExt::Source> sources;
    SWExt::EvalContext ctx;
    this->GatherSources(pFirer, sources, ctx);

    for (auto const& src : sources)
    {
        if (!SWExt::IsEligible(this->Inhibitors, src, cell.X, cell.Y, ctx))
            continue;

        const int override_ = this->Inhibitors.OverrideRangeFor(src.TypeIndex);
        const int base      = override_ >= 0 ? override_ : src.FallbackRange;
        const int growth    = this->Inhibitors.Growth.DeltaAt(ctx.Frames);
        const int count     = SWExt::CountRatioFor(this->Inhibitors, src, ctx);
        const int final_    = SWExt::EffectiveRange(this->Inhibitors, src, ctx);

        // Cheap change-detector, not a hash with any security meaning. Computed
        // BEFORE printing so an unchanged explanation prints nothing at all.
        const int newSig =
            (final_ * 1000003) ^ (count * 31) ^ (src.CellX * 7) ^ src.CellY;

        if (newSig == sig)
            return;   // same answer as last time; say nothing

        sig = newSig;

        auto const pType = TechnoTypeClass::Array.GetItemOrDefault(src.TypeIndex);

        Debug::Log("[SuperWeaponExt]   blocked by %s at (%d,%d): radius %d "
                   "(base %d + growth %d + ratio %d*%d counted", 
                   pType ? pType->ID : "?", src.CellX, src.CellY,
                   final_, base, growth, this->Inhibitors.Ratio.PerUnit, count);
        Debug::Log(") [frame %d]\n", ctx.Frames);
        return;   // one explanation is enough
    }
}

void SWTypeExt::LogCursorDenial(SuperWeaponTypeClass* pType, const CellStruct& cell)
{
    if (!pType)
        return;

    auto const pExt = SWTypeExt::ExtMap.Find(pType);
    auto const pPlayer = HouseClass::CurrentPlayer;
    if (!pExt || !pPlayer)
        return;

    Debug::Log("[SuperWeaponExt] %s refused at (%d,%d)\n",
               pType->ID, cell.X, cell.Y);
    pExt->LogDenial(pPlayer, cell);
}

// =============================================================================
// Layer 3's refusal log.
//
// ⚠ WHY THIS EXISTS, having already written LogCursorDenial for Layer 2.
//
// Layer 2 (the click veto at 0x4AC21C) logs one line per refused click. But once
// Layer 3 has refused the CURSOR, the click never resolves a superweapon at all,
// so Layer 2's hook reads a null pSWType and returns before reaching its log.
// The net effect: a player-facing refusal — the thing actually observable in
// game — produced ZERO log lines across three sessions, while the feature was
// demonstrably working. The diagnostic was on a path those refusals never take.
//
// This is the same mistake twice: the first version logged from the LAUNCH path,
// which Layer 2 pre-empts; the fix moved it to Layer 2, which Layer 3 pre-empts.
// The rule that actually generalises: put the diagnostic on the layer that
// PRODUCES THE BEHAVIOUR, not on the one that conceptually owns the decision.
//
// Throttled two ways, because GetAction runs on every mouse move:
//   1. at most once every 15 frames (~1s), bounding the cost of re-gathering;
//   2. then only when the EXPLANATION changes — so parking a tank next to the
//      power plant prints exactly one new line showing the new radius.
// =============================================================================
void SWTypeExt::LogCursorRefusal(SuperWeaponTypeClass* pType, const CellStruct& cell)
{
    if (!pType)
        return;

    auto const pExt = SWTypeExt::ExtMap.Find(pType);
    auto const pPlayer = HouseClass::CurrentPlayer;
    if (!pExt || !pPlayer)
        return;

    static int s_lastFrame = -1000;
    static int s_lastSig   = 0;

    const int frame = Unsorted::CurrentFrame;
    if (frame - s_lastFrame < 15 && frame >= s_lastFrame)
        return;
    s_lastFrame = frame;

    pExt->LogDenialSig(pPlayer, cell, s_lastSig);
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
namespace
{
    // Screen point (VIEW-RELATIVE) -> the cell the player is actually looking at.
    //
    // ⚠ Do NOT do this with TacticalClass::ClientToCoords. That maps a screen
    // point to world coordinates as if the ground were flat, so on raised
    // terrain it returns a cell offset toward the north — by roughly one cell
    // per height level, which in play looked like a 1-4 cell error that varied
    // with the terrain. CoordsToScreen shows why: it draws at
    // `y - AdjustForZ(coord.Z)`, and the naive inverse cannot undo a Z it does
    // not know.
    //
    // DisplayClass::ProcessClickCoords is the engine's own resolver — it is what
    // the real mouse handler calls at 0x4AACD4, and it walks the terrain to find
    // the cell actually under the pixel, height and bridges included.
    CellStruct ScreenPointToCell(Point2D point)
    {
        CellStruct cell = CellStruct::Empty;
        CoordStruct coord {};
        ObjectClass* pTarget = nullptr;
        BYTE unk1 = 0;
        BYTE unk2 = 0;

        DisplayClass::Instance.ProcessClickCoords(
            &point, &cell, &coord, &pTarget, &unk1, &unk2);

        return cell;
    }
}

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
    {
        // ViewBounds is already the tactical viewport, so its centre is a
        // view-relative point — exactly what ScreenPointToCell wants.
        const Point2D centre{ DSurface::ViewBounds.Width / 2,
                              DSurface::ViewBounds.Height / 2 };
        return ScreenPointToCell(centre);
    }

    case M::Mouse:
    default:
        if (auto const pMouse = WWMouseClass::Instance)
        {
            Point2D pt{};
            pMouse->GetCoords(&pt);
            // Mouse coords are screen-absolute; make them view-relative the same
            // way the engine's own mouse handler does at 0x4AAC92.
            pt.X -= DSurface::ViewBounds.X;
            pt.Y -= DSurface::ViewBounds.Y;
            return ScreenPointToCell(pt);
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

        Debug::Log("[SuperWeaponExt] hotkey slot %d fired %s at cell (%d,%d)\n",
                   index, pType->ID, cell.X, cell.Y);

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

// Hooks the `ret 0xc` that ends SuperWeaponTypeClass::Save, so our static data
// is written after the game has written its own.
//
// Size 3 is the true length of that `ret`. Syringe stamps 5 bytes regardless and
// would resume at 0x6CE8EF, which is NOT where control goes: the stub copies the
// 3-byte `ret`, and the `ret` executes and returns to the caller before the
// stub's trailing jump is ever reached. The 2 bytes the patch spills past the
// `ret` are compiler NOP padding (0x6CE8ED-0x6CE8EE) ahead of the next function
// at 0x6CE8F0 -- alignment filler, not code or data.
//
// Contrast 0x4F8361, where the identical pattern spills into a live switch jump
// table. The shape of the hook is the same; only the bytes behind it differ.
//
// syringe-hook-ok: stolen bytes are a `ret`; the 2 spilled bytes are NOP padding
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

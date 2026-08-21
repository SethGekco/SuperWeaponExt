#pragma once
/*
 * SuperWeaponExt — SuperWeaponTypeClass extension.
 *
 * Holds the parsed inhibitor/designator rules for one superweapon. The rules
 * themselves are engine-free (src/SW/Constraint.h) so they can be unit-tested
 * off-target; this class is only the INI front-end and the container plumbing.
 */
#include <SW/Constraint.h>

#include <GeneralStructures.h>   // CellStruct — SuperWeaponTypeClass.h does not pull it directly
#include <SuperWeaponTypeClass.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

class HouseClass;

class SWTypeExt
{
public:
    using base_type = SuperWeaponTypeClass;

    // Distinct from Antares' SWType canary (0x66666666) and every other in the
    // wild — see Ext/TechnoType/Body.h for the survey.
    static constexpr DWORD Canary = 0x5B5B5B5B;

    class ExtData final : public Extension<SuperWeaponTypeClass>
    {
    public:
        SWExt::Rule Inhibitors;
        SWExt::Rule Designators;

        explicit ExtData(SuperWeaponTypeClass* pOwner)
            : Extension<SuperWeaponTypeClass>(pOwner)
            , Inhibitors{}
            , Designators{}
        { }

        virtual ~ExtData() = default;

        virtual void LoadFromINIFile(CCINIClass* pINI) override;

        // Only ints are stored — nothing to invalidate.
        virtual void InvalidatePointer(void*, bool) override { }

        virtual void LoadFromStream(PhobosStreamReader& stm) override;
        virtual void SaveToStream(PhobosStreamWriter& stm) override;

        // True when the modder configured neither role, i.e. this superweapon is
        // not ours to police and every hook must bail out immediately.
        bool IsConfigured() const
        {
            return this->Inhibitors.Active() || this->Designators.Active();
        }

        // THE decision. Walks TechnoClass::Array, reduces each techno to a
        // SWExt::Source, and asks the pure core. Returns true when the shot is
        // permitted.
        bool AllowsFireAt(HouseClass* pFirer, const CellStruct& cell) const;

    private:
        template <typename T> void Serialize(T& stm);
    };

    class ExtContainer final : public Container<SWTypeExt>
    {
    public:
        ExtContainer();
        ~ExtContainer();
    };

    static ExtContainer ExtMap;
};

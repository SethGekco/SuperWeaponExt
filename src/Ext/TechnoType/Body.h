#pragma once
/*
 * SuperWeaponExt — TechnoTypeClass extension.
 *
 * Holds the veterancy-tiered inhibitor/designator radii for one TechnoType.
 *
 * WHY A SEPARATE TAG NAMESPACE: Antares already reads InhibitorRange /
 * DesignatorRange into its OWN TechnoType ext, which we cannot see. We do not
 * try to read or override its values — we parse our own SWExt.* keys into our
 * own container. A mod using our system leaves Antares' SW.Inhibitors empty,
 * which makes its check a no-op. See FINDINGS.md §0.
 */
#include <SW/Constraint.h>

#include <TechnoTypeClass.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

class TechnoTypeExt
{
public:
    using base_type = TechnoTypeClass;

    // Canary must not collide with any in the wild. Known: Phobos/Antares
    // 0x11111111 (BuildingType), 0x44444444 (TechnoType), 0x22222222 (Warhead),
    // 0x66666666 (Antares SWType), 0xAFFEAFFE (HouseType), 0xA7A7A7A7 +
    // 0x9B9B9B9B (AITriggerType), 0xB2B2B2B2 (PrerequisiteExt), 0x0ACADE0n
    // (AcademyExt), 0x50DEC77 (SquadExt).
    static constexpr DWORD Canary = 0x5C5C5C5C;

    class ExtData final : public Extension<TechnoTypeClass>
    {
    public:
        SWExt::RangeSpec InhibitorRange;
        SWExt::RangeSpec DesignatorRange;

        explicit ExtData(TechnoTypeClass* pOwner)
            : Extension<TechnoTypeClass>(pOwner)
            , InhibitorRange{}
            , DesignatorRange{}
        { }

        virtual ~ExtData() = default;

        virtual void LoadFromINIFile(CCINIClass* pINI) override;

        // We store only ints — nothing to invalidate.
        virtual void InvalidatePointer(void*, bool) override { }

        virtual void LoadFromStream(PhobosStreamReader& stm) override;
        virtual void SaveToStream(PhobosStreamWriter& stm) override;

    private:
        template <typename T> void Serialize(T& stm);
    };

    class ExtContainer final : public Container<TechnoTypeExt>
    {
    public:
        ExtContainer();
        ~ExtContainer();
    };

    static ExtContainer ExtMap;
};

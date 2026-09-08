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
#include <SW/StandingOrder.h>

class SuperWeaponTypeClass;

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

        // Per-AircraftType override of [General] ParadropRadius, in leptons.
        // <0 means "not overridden". Only meaningful on AircraftTypes; the
        // engine compares a paradrop plane's distance-to-target against this to
        // decide when to START dropping (see Ext/SWType/Hooks.ParaDrop.cpp).
        int ParadropRadius;

        // Persistent "when idle, head toward X" rule. Costs nothing unless a
        // modder configures it — Mode defaults to None and the tick skips every
        // type whose order is inactive.
        SWExt::StandingOrder Order;

        // "Use a different weapon against a designator/inhibitor."
        // Weapon INDEX (0/1/...), <0 = not set. The optional scoped SW is the
        // "honor indexes" case: restrict the test to one superweapon's list
        // rather than the union across all of them.
        struct WeaponVsSpec
        {
            int VsInhibitor  = -1;
            int VsDesignator = -1;

            SuperWeaponTypeClass* InhibitorSW  = nullptr;
            SuperWeaponTypeClass* DesignatorSW = nullptr;

            bool Active() const
            {
                return this->VsInhibitor >= 0 || this->VsDesignator >= 0;
            }
        };

        WeaponVsSpec WeaponVs;

        // Cache for UnifiedIndex(). <0 = not computed yet.
        int CachedUnifiedIndex = -1;

        explicit ExtData(TechnoTypeClass* pOwner)
            : Extension<TechnoTypeClass>(pOwner)
            , InhibitorRange{}
            , DesignatorRange{}
            , ParadropRadius(-1)
            , Order{}
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

    // ⚠ COLLISION-FREE TYPE IDENTITY. Use this, NEVER GetArrayIndex(), whenever
    // a TechnoType index is compared against another TechnoType index.
    //
    // GetArrayIndex() is PER-SUBCLASS: it returns the index within
    // UnitTypeClass::Array / BuildingTypeClass::Array / InfantryTypeClass::Array
    // / AircraftTypeClass::Array, which are four separate arrays. So indices
    // COLLIDE across categories -- in a stock rules file APOC (vehicle 2) and
    // GACNST (building 2) share an index, as do HTNK and GAPILE, and MTNK and
    // NAPOWR. Phobos gives the game away by keeping four separate counter arrays
    // for exactly this reason (Ext/House/Body.cpp AddToLimboTracking).
    //
    // This returns the index within the UNIFIED TechnoTypeClass::Array, which is
    // unique across every techno type. Cached per type, because FindItemIndex is
    // a linear scan and the runtime callers are per-object-per-frame.
    //
    // Found in game: an inhibitor ratio counting "tanks" was silently counting
    // the player's Construction Yard and barracks, so the radius grew as the
    // base was built and looked like a time-based effect.
    static int UnifiedIndex(TechnoTypeClass* pType);

    static ExtContainer ExtMap;
};

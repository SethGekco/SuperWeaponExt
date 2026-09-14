/*
 * SuperWeaponExt — a building grants its SuperWeapon= to OTHER houses.
 *
 * Rex's case: a GAAIRC carrying `SuperWeapon=ENEMYNUKE`, where his ENEMIES get
 * the cameo and fire it. Vanilla and every framework grant a building's
 * superweapon to its own owner and nobody else.
 *
 * ============================================================================
 * WHY THIS IS NOT A SIMPLE "GRANT IT" — AND WHY THE TIMER CODE EXISTS
 * ============================================================================
 *
 * Antares REPLACES superweapon availability wholesale:
 *
 *   0x50AF10  HouseClass::UpdateSuperWeaponsOwned        -> returns 0x50B1CA
 *   0x50B1D0  HouseClass::UpdateSuperWeaponsUnavailable
 *   0x6CB7B0  SuperClass::Lose                           -> returns 0x6CB810
 *
 * Its GetSuperWeaponStatuses() walks `pHouse->Buildings` — the house's OWN
 * buildings — and anything it cannot justify from that list gets Lose()d on the
 * next update. So a naive Grant() is revoked within a frame.
 *
 * Contending for those addresses is not an option either: Antares' handlers are
 * full replacements returning non-zero, so a same-address chain behind them
 * never runs (and which handler is "behind" depends on injection order, which is
 * not something to build on).
 *
 * The cooperative answer is to stop fighting the computation and simply RESTORE
 * what it removes, every tick, from the frame seat we already own:
 *
 *   - while the superweapon is present, snapshot its recharge timer;
 *   - when Antares has removed it but our rule still says it belongs there,
 *     Grant() it again and put the snapshotted timer back.
 *
 * Antares' Lose() clears IsPresent/IsReady but does NOT touch RechargeTimer;
 * vanilla Grant() restarts it. Without the restore the charge would reset every
 * single tick and the superweapon would never become ready — which is exactly
 * the failure this would have shipped with.
 *
 * Uses the existing TechnoTypeExt container rather than a new BuildingType one:
 * BuildingTypeClass derives from TechnoTypeClass, and our container hooks the
 * shared TechnoTypeClass CTOR, so buildings already carry ext data.
 *
 * ⚠ DETERMINISM. Everything here is synced state: the building list, house
 * relations and the frame counter. No local player, cursor or view. Every client
 * grants and restores identically.
 */
#include "StandingOrders.h"

#include <Ext/SWType/Body.h>
#include <Ext/TechnoType/Body.h>

#include <SW/Constraint.h>

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <Fundamentals.h>
#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <Utilities/Debug.h>

#include <vector>

namespace
{
    // One cross-granted (house, superweapon) pair and the charge we are keeping
    // alive for it across Antares' revoke/regrant churn.
    struct GrantRecord
    {
        int HouseIndex;
        int SWIndex;
        int StartTime;   // snapshot of RechargeTimer
        int TimeLeft;
        bool Seen;       // still justified this tick?
        bool Logged;
    };

    std::vector<GrantRecord> g_grants;

    GrantRecord* Find(int houseIdx, int swIdx)
    {
        for (auto& g : g_grants)
            if (g.HouseIndex == houseIdx && g.SWIndex == swIdx)
                return &g;
        return nullptr;
    }

    SWExt::Relation RelationOf(HouseClass* pFrom, HouseClass* pTo)
    {
        if (pFrom == pTo)
            return SWExt::Relation::Owner;
        return pFrom->IsAlliedWith(pTo) ? SWExt::Relation::Allies
                                        : SWExt::Relation::Enemies;
    }

    // Is this building currently able to source a superweapon at all? Mirrors
    // the conditions Antares applies to a house's own buildings, so a
    // cross-granted superweapon behaves like a normally granted one.
    bool BuildingCanSource(BuildingClass* pBld)
    {
        return pBld
            && pBld->IsAlive
            && pBld->Health > 0
            && !pBld->InLimbo
            && pBld->HasPower
            && !pBld->IsUnderEMP()
            && pBld->CurrentMission != Mission::Construction
            && pBld->CurrentMission != Mission::Selling;
    }
}

void SWExt::StandingOrders::TickCrossHouseGrants()
{
    // Cheap global early-out: nothing configured means one flag test per frame.
    static bool s_any = false;
    static bool s_checked = false;

    if (!s_checked)
    {
        s_checked = true;
        for (int i = 0; i < BuildingTypeClass::Array.Count; ++i)
        {
            auto const pType = BuildingTypeClass::Array.GetItem(i);
            if (!pType)
                continue;

            auto const pExt = TechnoTypeExt::ExtMap.Find(pType);
            if (pExt && pExt->SuperWeaponGrantTo != SWExt::Relation::None)
            {
                s_any = true;
                break;
            }
        }
    }

    if (!s_any)
        return;

    for (auto& g : g_grants)
        g.Seen = false;

    // ---------------------------------------------------------------------
    // Work out who SHOULD have what, from every qualifying building on the map.
    // ---------------------------------------------------------------------
    for (int i = 0; i < BuildingClass::Array.Count; ++i)
    {
        BuildingClass* const pBld = BuildingClass::Array.GetItem(i);
        if (!BuildingCanSource(pBld))
            continue;

        auto const pType = pBld->Type;
        if (!pType)
            continue;

        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        if (!pTypeExt || pTypeExt->SuperWeaponGrantTo == SWExt::Relation::None)
            continue;

        HouseClass* const pOwner = pBld->Owner;
        if (!pOwner)
            continue;

        const int swIndices[2] = { pType->SuperWeapon, pType->SuperWeapon2 };

        for (int s = 0; s < 2; ++s)
        {
            const int swIdx = swIndices[s];
            if (swIdx < 0)
                continue;

            for (int h = 0; h < HouseClass::Array.Count; ++h)
            {
                HouseClass* const pHouse = HouseClass::Array.GetItem(h);
                if (!pHouse || pHouse->Defeated || pHouse->IsObserver())
                    continue;

                // The OWNER already gets this the normal way; granting it again
                // here would fight the engine for no benefit.
                if (pHouse == pOwner)
                    continue;

                if (!SWExt::Matches(pTypeExt->SuperWeaponGrantTo,
                                    RelationOf(pOwner, pHouse)))
                {
                    continue;
                }

                SuperClass* const pSuper = pHouse->Supers.GetItemOrDefault(swIdx);
                if (!pSuper)
                    continue;

                GrantRecord* pRec = Find(pHouse->ArrayIndex, swIdx);
                if (!pRec)
                {
                    g_grants.push_back(GrantRecord{
                        pHouse->ArrayIndex, swIdx, -1, 0, true, false });
                    pRec = &g_grants.back();
                }
                pRec->Seen = true;

                if (pSuper->IsPresent)
                {
                    // Keep the charge we will need to restore after the next
                    // time Antares takes it away.
                    pRec->StartTime = pSuper->RechargeTimer.StartTime;
                    pRec->TimeLeft  = pSuper->RechargeTimer.TimeLeft;
                    continue;
                }

                // Antares removed it (no owned building justifies it). Put it
                // back, then restore the charge Grant() just reset.
                pSuper->Grant(false, false, false);

                if (pRec->StartTime != -1 || pRec->TimeLeft != 0)
                {
                    pSuper->RechargeTimer.StartTime = pRec->StartTime;
                    pSuper->RechargeTimer.TimeLeft  = pRec->TimeLeft;
                }

                if (!pRec->Logged)
                {
                    pRec->Logged = true;
                    Debug::Log("[SuperWeaponExt] %s cross-granted to house %d "
                               "(%s) by %s owned by house %d\n",
                               pSuper->Type ? pSuper->Type->ID : "?",
                               pHouse->ArrayIndex,
                               pHouse->Type ? pHouse->Type->ID : "?",
                               pType->ID, pOwner->ArrayIndex);
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Drop records nothing justifies any more. We do NOT revoke the superweapon
    // ourselves — we simply stop restoring it, and Antares' own update removes
    // it on its next pass. Letting the incumbent do the revoking keeps the
    // sidebar and tech-tree bookkeeping in its hands rather than ours.
    // ---------------------------------------------------------------------
    for (std::size_t i = 0; i < g_grants.size(); )
    {
        if (!g_grants[i].Seen)
        {
            Debug::Log("[SuperWeaponExt] cross-grant of SW %d to house %d no "
                       "longer justified; releasing it\n",
                       g_grants[i].SWIndex, g_grants[i].HouseIndex);

            g_grants[i] = g_grants.back();
            g_grants.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

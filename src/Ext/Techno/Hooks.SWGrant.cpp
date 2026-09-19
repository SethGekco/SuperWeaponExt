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
 * ⚠ We honour the RECIPIENT's own eligibility via SW.AllowPlayer / SW.AllowAI.
 * Antares enforces those inside IsAvailable(), which has no address we can call,
 * so those two tags are re-read into our own ext and applied here. Other parts of
 * IsAvailable (RequiredHouses, AuxBuildings, Shots) are NOT yet mirrored — a
 * cross-granted superweapon can still bypass those. Documented, not silent.
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
    //
    // ⚠ RE-CHECKED periodically, not once. A one-shot check latches whatever was
    // true on the very first tick, so if that fired before the rules ext was
    // populated the feature would stay silently disabled for the whole session
    // with no way to tell from the log. The standing-order tick already learned
    // this; this one did not, originally.
    static bool s_any = false;
    static int  s_lastCheck = -100000;

    const int frame = Unsorted::CurrentFrame;

    if (frame < s_lastCheck || frame - s_lastCheck >= 900)
    {
        s_lastCheck = frame;

        const bool was = s_any;
        s_any = false;

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

        if (s_any != was)
        {
            Debug::Log("[SuperWeaponExt] cross-grant scan %s (frame %d)\n",
                       s_any ? "ENABLED" : "disabled", frame);
        }
    }

    if (!s_any)
        return;

    // Survey counters — why a configured rule produced no grants is otherwise
    // invisible, and "nothing in the log" has cost this project several rounds.
    int surveyConfigured = 0, surveyNotSourcing = 0;
    int surveyBuildings = 0, surveyNoSW = 0, surveyHousesSeen = 0;
    int surveySkipRelation = 0, surveySkipEligible = 0, surveySkipNoSuper = 0;

    for (auto& g : g_grants)
        g.Seen = false;

    // ---------------------------------------------------------------------
    // Work out who SHOULD have what, from every qualifying building on the map.
    // ---------------------------------------------------------------------
    for (int i = 0; i < BuildingClass::Array.Count; ++i)
    {
        BuildingClass* const pBld = BuildingClass::Array.GetItem(i);
        if (!pBld)
            continue;

        auto const pType = pBld->Type;
        if (!pType)
            continue;

        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        if (!pTypeExt || pTypeExt->SuperWeaponGrantTo == SWExt::Relation::None)
            continue;

        // Counted BEFORE the sourcing test, so the survey can tell "no such
        // building on the map" apart from "it is there but unpowered / still
        // being built" — two very different problems that would otherwise
        // produce the same empty log.
        ++surveyConfigured;

        if (!BuildingCanSource(pBld))
        {
            ++surveyNotSourcing;
            continue;
        }

        ++surveyBuildings;

        HouseClass* const pOwner = pBld->Owner;
        if (!pOwner)
            continue;

        const int swIndices[2] = { pType->SuperWeapon, pType->SuperWeapon2 };

        if (swIndices[0] < 0 && swIndices[1] < 0)
            ++surveyNoSW;

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

                ++surveyHousesSeen;

                if (!SWExt::Matches(pTypeExt->SuperWeaponGrantTo,
                                    RelationOf(pOwner, pHouse)))
                {
                    ++surveySkipRelation;
                    continue;
                }

                SuperClass* const pSuper = pHouse->Supers.GetItemOrDefault(swIdx);
                if (!pSuper || !pSuper->Type)
                {
                    ++surveySkipNoSuper;
                    continue;
                }

                // Honour the recipient's own eligibility. Antares gates presence
                // on IsAvailable(), which is C++ with no address we can call, so
                // we read the same SW.AllowPlayer / SW.AllowAI tags and apply
                // them ourselves. Without this a cross-grant would be a side
                // door around rules the modder already wrote.
                if (auto const pSWExt = SWTypeExt::ExtMap.Find(pSuper->Type))
                {
                    const bool human = pHouse->IsControlledByHuman();
                    if (human ? !pSWExt->AllowPlayer : !pSWExt->AllowAI)
                    {
                        ++surveySkipEligible;
                        continue;
                    }
                }

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

    // Report the survey while nothing has been granted, throttled to once a
    // minute. This is the line that says WHICH filter ate the grant, instead of
    // leaving an empty log to be guessed at.
    if (g_grants.empty())
    {
        static int s_lastSurvey = -100000;
        if (frame < s_lastSurvey || frame - s_lastSurvey >= 900)
        {
            s_lastSurvey = frame;
            Debug::Log("[SuperWeaponExt] cross-grant survey: %d configured "
                       "building(s) on map, %d not currently sourcing "
                       "(power/EMP/building/selling), %d sourcing, %d with no "
                       "SuperWeapon=, %d house(s) considered; skipped %d on "
                       "relation, %d on eligibility, %d with no SuperClass\n",
                       surveyConfigured, surveyNotSourcing, surveyBuildings,
                       surveyNoSW, surveyHousesSeen,
                       surveySkipRelation, surveySkipEligible, surveySkipNoSuper);
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

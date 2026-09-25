/*
 * SuperWeaponExt — auto-fire conditions.
 *
 * The decision lives in SW/AutoFire.h and is engine-free; this file only feeds
 * it synced facts and performs the launch.
 *
 * ============================================================================
 * ⚠ FIRE DIRECTLY, DO NOT QUEUE AN EVENT
 * ============================================================================
 *
 * The obvious way to fire a superweapon is to queue EventType::SpecialPlace,
 * which is what our hotkey path does — correctly, because a hotkey press
 * happens on ONE client and the event distributes it.
 *
 * An auto-fire condition is the opposite case. It is computed from synced state,
 * so it becomes true on EVERY client on the SAME frame. If each client queued an
 * event, an N-player game would enqueue N events and fire the superweapon N
 * times. Calling Fire_SW directly means each client performs the one launch
 * locally and they stay in step — which is exactly how the engine's own AI and
 * Antares' SW.AutoFire both do it.
 *
 * ============================================================================
 * WHY A LAUNCH BLACKBOARD
 * ============================================================================
 *
 * "Fire when an enemy fires theirs" needs to know what fired this frame. The
 * Fire_SW hook already sees every launch in the game, so it records them here
 * and the tick drains the list. Recording rather than reacting inline matters:
 * reacting inside Fire_SW would fire a superweapon from inside another
 * superweapon's launch, re-entering the same hook.
 */
#include "Body.h"

#include <Ext/Techno/StandingOrders.h>

#include <Fundamentals.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <Utilities/Debug.h>

#include <vector>

namespace
{
    // Launches seen this frame: (house index, SW index). Cleared each tick.
    struct Launch
    {
        int HouseIndex;
        int SWIndex;
    };

    std::vector<Launch> g_launches;

    // Per (house, SW) edge-detection state for the engine-free decision.
    struct StateRecord
    {
        int HouseIndex;
        int SWIndex;
        SWExt::AutoFireState State;
    };

    std::vector<StateRecord> g_states;

    SWExt::AutoFireState& StateFor(int houseIdx, int swIdx)
    {
        for (auto& r : g_states)
            if (r.HouseIndex == houseIdx && r.SWIndex == swIdx)
                return r.State;

        g_states.push_back(StateRecord{ houseIdx, swIdx, {} });
        return g_states.back().State;
    }

    // Defeat is a LEVEL (the flag stays set), so the momentary "was defeated
    // this frame" fact has to be derived by remembering the previous value.
    std::vector<char> g_wasDefeated;

    SWExt::Relation RelationOf(HouseClass* pFrom, HouseClass* pTo)
    {
        if (pFrom == pTo)
            return SWExt::Relation::Owner;
        return pFrom->IsAlliedWith(pTo) ? SWExt::Relation::Allies
                                        : SWExt::Relation::Enemies;
    }

    int CountryOf(HouseClass* pHouse)
    {
        return (pHouse && pHouse->Type) ? pHouse->Type->ArrayIndex : -1;
    }

    // Credits of the first house matching `rel` (and the country filter), or <0
    // if there is none.
    //
    // First-match rather than max/sum on purpose: it is the only choice that is
    // stable and obvious to a modder. A sum would make "an enemy has 5000" mean
    // something different in a 2-player and an 8-player game.
    int WatchedMoney(HouseClass* pOwner, SWExt::Relation rel,
                     const std::vector<int>& countries)
    {
        for (int i = 0; i < HouseClass::Array.Count; ++i)
        {
            HouseClass* const pHouse = HouseClass::Array.GetItem(i);
            if (!pHouse || pHouse->Defeated || pHouse->IsObserver())
                continue;
            if (!SWExt::Matches(rel, RelationOf(pOwner, pHouse)))
                continue;
            if (!SWExt::CountryAllowed(countries, CountryOf(pHouse)))
                continue;

            return pHouse->Available_Money();
        }
        return -1;
    }
}

void SWTypeExt::RecordLaunch(HouseClass* pHouse, int swIndex)
{
    if (!pHouse || swIndex < 0)
        return;

    g_launches.push_back(Launch{ pHouse->ArrayIndex, swIndex });
}

void SWTypeExt::TickAutoFire()
{
    const int frame = Unsorted::CurrentFrame;

    // Cheap global early-out, re-checked periodically rather than latched once
    // — a one-shot check that ran before the rules ext was populated would
    // disable the feature for the whole session with nothing in the log.
    static bool s_any = false;
    static int  s_lastCheck = -100000;

    if (frame < s_lastCheck || frame - s_lastCheck >= 900)
    {
        s_lastCheck = frame;
        s_any = false;

        for (int i = 0; i < SuperWeaponTypeClass::Array.Count; ++i)
        {
            auto const pType = SuperWeaponTypeClass::Array.GetItem(i);
            if (!pType)
                continue;

            auto const pExt = SWTypeExt::ExtMap.Find(pType);
            if (pExt && pExt->AutoFire.Active())
            {
                s_any = true;
                break;
            }
        }
    }

    if (!s_any)
    {
        g_launches.clear();
        return;
    }

    // --- derive "defeated THIS frame" from the level flag ------------------
    const int houseCount = HouseClass::Array.Count;
    if (static_cast<int>(g_wasDefeated.size()) < houseCount)
        g_wasDefeated.resize(houseCount, 0);

    std::vector<char> defeatedNow(houseCount, 0);
    int aliveHouses = 0;

    for (int i = 0; i < houseCount; ++i)
    {
        HouseClass* const pHouse = HouseClass::Array.GetItem(i);
        if (!pHouse || pHouse->IsObserver())
            continue;

        const bool def = pHouse->Defeated;
        defeatedNow[i] = (def && !g_wasDefeated[i]) ? 1 : 0;
        g_wasDefeated[i] = def ? 1 : 0;

        if (!def)
            ++aliveHouses;
    }

    // --- evaluate every (house, superweapon) with a rule --------------------
    for (int h = 0; h < houseCount; ++h)
    {
        HouseClass* const pOwner = HouseClass::Array.GetItem(h);
        if (!pOwner || pOwner->Defeated || pOwner->IsObserver())
            continue;

        for (int s = 0; s < pOwner->Supers.Count; ++s)
        {
            SuperClass* const pSuper = pOwner->Supers.GetItem(s);
            if (!pSuper || !pSuper->IsPresent || !pSuper->Type)
                continue;

            auto const pExt = SWTypeExt::ExtMap.Find(pSuper->Type);
            if (!pExt || !pExt->AutoFire.Active())
                continue;

            const auto& rule = pExt->AutoFire;

            SWExt::AutoFireInputs in;
            in.Frame   = frame;
            in.Charged = pSuper->CanFire() != 0;

            // Did a watched superweapon fire, by a house matching the relation?
            for (auto const& l : g_launches)
            {
                if (!rule.WatchesSW(l.SWIndex))
                    continue;

                HouseClass* const pFirer = HouseClass::Array.GetItemOrDefault(l.HouseIndex);
                if (!pFirer)
                    continue;

                // Our own auto-fire must not retrigger itself.
                if (pFirer == pOwner && l.SWIndex == s)
                    continue;

                if (!SWExt::CountryAllowed(rule.OnSWFiredCountries, CountryOf(pFirer)))
                    continue;

                if (SWExt::Matches(rule.OnSWFiredHouse, RelationOf(pOwner, pFirer)))
                {
                    in.SWFired = true;
                    break;
                }
            }

            if (rule.OnDefeat)
            {
                for (int d = 0; d < houseCount; ++d)
                {
                    if (!defeatedNow[d])
                        continue;

                    HouseClass* const pDead = HouseClass::Array.GetItemOrDefault(d);
                    if (!SWExt::CountryAllowed(rule.OnDefeatCountries, CountryOf(pDead)))
                        continue;

                    if (pDead && SWExt::Matches(rule.OnDefeatHouse,
                                                RelationOf(pOwner, pDead)))
                    {
                        in.Defeated = true;
                        break;
                    }
                }
            }

            // "Victory" = the owner is the last house standing. Deliberately
            // narrow; the engine's own win handling happens elsewhere and this
            // only needs a synced, unambiguous moment.
            if (rule.OnVictory)
                in.Victory = (aliveHouses == 1 && !pOwner->Defeated);

            if (rule.HasMoneyBand())
                in.Money = WatchedMoney(pOwner, rule.OnMoneyHouse, rule.OnMoneyCountries);

            auto& state = StateFor(pOwner->ArrayIndex, s);
            if (!SWExt::ShouldAutoFire(rule, in, state))
                continue;

            // Where to put it. AI targeting belongs to Antares, so this uses the
            // superweapon's own resolver — the same one the dedicated hotkey
            // path uses — and falls back to the owner's base.
            const CellStruct cell = pExt->ResolveHotkeyCell(pOwner);

            Debug::Log("[SuperWeaponExt] auto-fire %s for house %d (%s) at "
                       "(%d,%d): swFired %d defeat %d victory %d money %d\n",
                       pSuper->Type->ID, pOwner->ArrayIndex,
                       pOwner->Type ? pOwner->Type->ID : "?",
                       cell.X, cell.Y, in.SWFired ? 1 : 0, in.Defeated ? 1 : 0,
                       in.Victory ? 1 : 0, in.Money);

            // ⚠ Direct call, NOT a queued event — see the header comment.
            // This also re-enters our own Fire_SW hook, which is intended: the
            // launch is then subject to inhibitors, money gates and everything
            // else, exactly as a manual shot would be.
            pOwner->Fire_SW(s, cell);
        }
    }

    // Drained once per frame, after every rule has had a chance to see them.
    g_launches.clear();
}

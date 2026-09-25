#pragma once
/*
 * SuperWeaponExt — fire a superweapon automatically when something happens.
 *
 * ENGINE-FREE, like Constraint.h / Money.h / Formation.h / StandingOrder.h.
 *
 * WHAT ANTARES ALREADY DOES, so this does not rebuild it: `SW.AutoFire=yes`
 * means "fire as soon as you are charged", with AI targeting and no further
 * condition. What is missing is a REASON — fire when an enemy fires theirs,
 * when somebody is rich, when a house dies, when you win.
 *
 * ============================================================================
 * MOMENTARY vs LEVEL CONDITIONS — the distinction this whole file turns on
 * ============================================================================
 *
 * "An enemy fired a superweapon", "a house was defeated" and "you won" are
 * MOMENTARY: true for a single frame, so they can fire immediately.
 *
 * "An enemy holds between 5000 and 9000 credits" is a LEVEL: true for thousands
 * of consecutive frames. Firing on the level would launch every frame it holds.
 * So a level condition fires only on its RISING EDGE — the frame it becomes
 * true — and re-arms when it goes false again.
 *
 * Getting that wrong is not subtle: it is a superweapon firing fifteen times a
 * second for as long as an enemy happens to be solvent.
 *
 * ⚠ DETERMINISM. Every input is synced simulation state (frame counter, house
 * credits, defeat flags, launches seen by the Fire_SW hook). The adapter fires
 * the launch DIRECTLY on every client rather than queueing a network event —
 * queueing would have each of N clients enqueue the same launch and produce N
 * launches. That is how the engine's own AI fires, and how Antares' AutoFire
 * does it.
 */
#include <vector>

#include "Constraint.h"   // Relation

namespace SWExt
{
    // Where an auto-fired shot lands.
    //
    // ⚠ EVERY MODE HERE IS SYNCED, and that is the entire point. Auto-fire is
    // evaluated on every client on the same frame, so if the target cell came
    // from the local cursor or the local view each client would launch at a
    // DIFFERENT cell — a guaranteed desync. The hotkey path may use the mouse
    // because a keypress happens on ONE client and travels as an event; auto-fire
    // may not. There is deliberately no Mouse or Screen option.
    enum class AutoFireTarget : unsigned char
    {
        Base = 0,   // the firing house's base centre (default)
        Trigger,    // where the superweapon that TRIGGERED us landed
        Cell,       // a fixed cell
        None,       // (0,0), for superweapons that ignore location
    };

    struct AutoFireRule
    {
        bool Enabled = false;

        AutoFireTarget Target = AutoFireTarget::Base;
        int TargetCellX = 0;
        int TargetCellY = 0;

        // --- momentary: one of these superweapons fired ---
        bool OnSWFiredAny = false;                       // any superweapon at all
        std::vector<int> OnSWFired;                      // SW type indices
        Relation OnSWFiredHouse = Relation::Enemies;     // ...by this relation to us
        std::vector<int> OnSWFiredCountries;             // ...and, if set, these countries

        // --- level: a watched house's credits sit inside a band ---
        int      OnMoneyMin   = -1;                      // <0 = unset
        int      OnMoneyMax   = -1;
        Relation OnMoneyHouse = Relation::Enemies;
        std::vector<int> OnMoneyCountries;

        // --- momentary ---
        bool     OnDefeat      = false;
        Relation OnDefeatHouse = Relation::Enemies;
        std::vector<int> OnDefeatCountries;
        bool     OnVictory     = false;                  // the OWNER won

        int  Cooldown       = 0;      // frames between auto-fires; 0 = recharge only
        bool RequireCharged = true;   // respect the recharge timer

        bool HasMoneyBand() const
        {
            return this->OnMoneyMin >= 0 || this->OnMoneyMax >= 0;
        }

        bool WatchesAnySW() const
        {
            return this->OnSWFiredAny || !this->OnSWFired.empty();
        }

        bool Active() const
        {
            return this->Enabled
                && (this->WatchesAnySW() || this->HasMoneyBand()
                    || this->OnDefeat || this->OnVictory);
        }

        bool WatchesSW(int swIndex) const
        {
            if (this->OnSWFiredAny)
                return true;
            for (int idx : this->OnSWFired)
                if (idx == swIndex)
                    return true;
            return false;
        }
    };

    // What the adapter observed this frame, for one (superweapon, house) pair.
    struct AutoFireInputs
    {
        bool SWFired  = false;   // a watched SW fired, by a matching house
        bool Defeated = false;   // a matching house was defeated THIS frame
        bool Victory  = false;   // the owner won THIS frame

        int  Money   = -1;       // watched house's credits; <0 = nobody to watch
        bool Charged = true;
        int  Frame   = 0;
    };

    // Carried between evaluations. One per (superweapon, house).
    struct AutoFireState
    {
        bool MoneyWasInBand = false;
        int  LastFiredFrame = -1000000;
    };

    // Country filter for a condition: "when the RUSSIANS are defeated", not just
    // "when an enemy is defeated".
    //
    // An EMPTY list means "any country" — so a relation alone keeps working
    // exactly as before and the filter is purely additive. A house whose country
    // cannot be resolved (index < 0) therefore passes an empty list but fails
    // any non-empty one, which is the conservative reading.
    inline bool CountryAllowed(const std::vector<int>& countries, int countryIndex)
    {
        if (countries.empty())
            return true;

        for (int idx : countries)
            if (idx == countryIndex)
                return true;

        return false;
    }

    inline bool MoneyInBand(const AutoFireRule& rule, int money)
    {
        if (!rule.HasMoneyBand() || money < 0)
            return false;
        if (rule.OnMoneyMin >= 0 && money < rule.OnMoneyMin)
            return false;
        if (rule.OnMoneyMax >= 0 && money > rule.OnMoneyMax)
            return false;
        return true;
    }

    // THE decision.
    //
    // ⚠ `state.MoneyWasInBand` is updated even when the answer is "no". A
    // rising edge that happens while the superweapon is still recharging is
    // CONSUMED, not remembered — otherwise the shot would fire late, long after
    // the condition that justified it had passed, which reads in game as a
    // superweapon going off for no reason.
    inline bool ShouldAutoFire(const AutoFireRule& rule, const AutoFireInputs& in,
                               AutoFireState& state)
    {
        const bool inBand    = MoneyInBand(rule, in.Money);
        const bool moneyEdge = inBand && !state.MoneyWasInBand;

        state.MoneyWasInBand = inBand;

        if (!rule.Active())
            return false;

        const bool triggered =
               (in.SWFired  && rule.WatchesAnySW())
            || (in.Defeated && rule.OnDefeat)
            || (in.Victory  && rule.OnVictory)
            || moneyEdge;

        if (!triggered)
            return false;

        if (rule.RequireCharged && !in.Charged)
            return false;

        if (rule.Cooldown > 0)
        {
            const int since = in.Frame - state.LastFiredFrame;
            // A backwards frame jump means a new scenario; treat it as elapsed
            // rather than locking the rule out for the rest of the match.
            if (since >= 0 && since < rule.Cooldown)
                return false;
        }

        state.LastFiredFrame = in.Frame;
        return true;
    }
}

#pragma once
/*
 * SuperWeaponExt — financial requirements for firing a superweapon.
 *
 * ENGINE-FREE, like Constraint.h / Formation.h / StandingOrder.h. The adapter
 * supplies the house's credits and whether it is human-controlled; this decides.
 *
 * WHY THIS EXISTS. The AI fires superweapons with no regard for its bank
 * balance, which is fine for free ones and wrong for anything that costs money.
 * Antares has `Money.Amount` (a transaction applied on launch) but performs NO
 * affordability check — it calls TransactMoney unguarded, so a costly
 * superweapon still fires and can drive the house negative. There is also no
 * minimum-credits requirement and no way to treat a human and an AI differently.
 *
 * Three independent quantities, deliberately kept separate:
 *
 *   Cost — credits DEDUCTED when the shot goes off. Also acts as a floor: a
 *          house that cannot pay cannot fire.
 *   Min  — credits the house must HAVE, and keeps. "Do not bother until you are
 *          comfortable" — the knob for teaching the AI restraint.
 *   Max  — credits the house must NOT exceed. Inverts the usual design: a
 *          desperation weapon available only while broke.
 *
 * Min/Max are a BAND, so Min=2000 Max=8000 is a superweapon usable only in the
 * mid-game. That is the "unusual design" case and it costs nothing extra.
 *
 * ⚠ DETERMINISM. House credits are synced simulation state and the launch check
 * runs downstream of the event queue, so every client reaches the same verdict on
 * the same frame. All arithmetic here is integer; nothing reads wall-clock time,
 * the local player, or the view.
 */
namespace SWExt
{
    // One controller's thresholds. <0 means "not set" for Min/Max so that zero
    // stays a meaningful value — Max=0 is "only when completely broke", which is
    // a legitimate setting and must not be confused with "no maximum".
    struct MoneySpec
    {
        int Cost = 0;    // credits deducted on fire; 0 = free
        int Min  = -1;   // must have at least this; <0 = no minimum
        int Max  = -1;   // must have at most this;  <0 = no maximum

        bool Active() const
        {
            return this->Cost != 0 || this->Min >= 0 || this->Max >= 0;
        }
    };

    // A full rule: a general setting plus optional per-controller overrides.
    struct MoneyRule
    {
        MoneySpec Both;    // applies unless the matching override is set
        MoneySpec Human;
        MoneySpec AI;

        bool Active() const
        {
            return this->Both.Active() || this->Human.Active() || this->AI.Active();
        }

        // Resolve the effective spec for one controller.
        //
        // Resolution is PER FIELD, not per spec: setting only SWExt.Cost.AI must
        // not silently discard a shared SWExt.RequiredMoney.Min. Each field falls
        // back to the general value independently.
        MoneySpec Resolve(bool isHuman) const
        {
            const MoneySpec& over = isHuman ? this->Human : this->AI;

            MoneySpec out = this->Both;

            if (over.Cost != 0)  out.Cost = over.Cost;
            if (over.Min  >= 0)  out.Min  = over.Min;
            if (over.Max  >= 0)  out.Max  = over.Max;

            return out;
        }

        int CostFor(bool isHuman) const { return this->Resolve(isHuman).Cost; }

        // May this house fire, given its current credits?
        bool Allows(int money, bool isHuman) const
        {
            const MoneySpec spec = this->Resolve(isHuman);

            // Cost is also an affordability floor. Checked separately from Min
            // because Min is money you KEEP and Cost is money you SPEND — a house
            // with exactly the cost may fire even with Min unset.
            if (spec.Cost > 0 && money < spec.Cost)
                return false;

            if (spec.Min >= 0 && money < spec.Min)
                return false;

            if (spec.Max >= 0 && money > spec.Max)
                return false;

            return true;
        }
    };
}

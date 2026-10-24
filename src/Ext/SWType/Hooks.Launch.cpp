/*
 * SuperWeaponExt — Layer 1: the authoritative launch veto.
 *
 * ============================================================================
 * HouseClass::Fire_SW @ 0x4FAE50
 * ============================================================================
 *
 * WHY THIS ADDRESS. Every superweapon launch in the game funnels through this
 * one function — 17 call sites, verified against a fresh disassembly:
 *
 *   0x4C78F3            RespondToEvent(SpecialPlace) — the network-synced
 *                       player-click path
 *   0x509ACD..0x50A480  HouseClass AI superweapon firing (10 sites, one per
 *                       vanilla SW)
 *   0x6EFDB4..0x6F030D  AI script / team-mission SW actions (5 sites)
 *   0x4FAE3F            internal tail-call
 *
 * The FRAMEWORK paths land here too, which is the whole point:
 *   - Antares' SWTypeExt::TryFire ends in pOwner->Fire_SW(...)
 *     (Hooks.Targeting.cpp:619), and YRpp's Fire_SW is JMP_THIS(0x4FAE50) —
 *     a direct call to this game address, not to anything inside Antares.
 *   - Phobos' SW-sidebar button and trigger actions 505/506 queue
 *     EventType::SpecialPlace, which arrives via 0x4C78F3.
 *
 * So one hook covers vanilla, Antares manual fire, Antares AI targeting, Phobos
 * sidebar, Phobos triggers and map triggers — without contending for a single
 * address any framework owns.
 *
 * REGISTRY STATUS: the ENTRY is unhooked by all six frameworks in the YR Hook
 * Encyclopedia (Ares, Antares, Phobos, Kratos, CnCNet-Spawner, AggressiveStance).
 * Antares takes 0x4FAE72 — that is +0x22 *inside* the function
 * (HouseClass_SWFire_PreDependent) and is not on the path from the entry.
 *
 * DESYNC PROPERTY: this sits downstream of the event queue, so every client
 * evaluates the veto on the same frame against the same game state. That is
 * inherited from the position, not from anything we do — which is exactly why
 * this layer, and not the cursor layer, is the authoritative one.
 *
 * ----------------------------------------------------------------------------
 * Stolen bytes (0x7), from the disassembly:
 *     4fae50:  53              push %ebx
 *     4fae51:  8b d9           mov  %ecx,%ebx
 *     4fae53:  8b 4c 24 08     mov  0x8(%esp),%ecx
 * 1 + 2 + 4 = 7, landing on an instruction boundary.
 *
 * At entry (nothing pushed yet):
 *     ECX      = HouseClass* this
 *     [esp+4]  = int idxSW
 *     [esp+8]  = CellStruct* pCoords
 *
 * ABORT PATH — the trap worth remembering. Do NOT jump to the real epilogue at
 * 0x4FAEED; it is `pop edi; pop esi; pop ebp; mov al,1; pop ebx; ret 8` and
 * would pop four registers that were never pushed. Jump to 0x4FAEF3 instead,
 * which is a bare `ret $8` — the correct cleanup for
 * __thiscall bool Fire_SW(int, CellStruct const&) from a pristine entry stack.
 */
#include "Body.h"

#include <Ext/Techno/StandingOrders.h>

#include <DisplayClass.h>
#include <Fundamentals.h>   // Unsorted::CurrentFrame, for the denial-log throttle
#include <HouseClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <CellClass.h>
#include <MapClass.h>        // MapCoordBounds / CoordinatesLegal, for the scatter
#include <ScenarioClass.h>   // the SYNCED in-match RNG
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Helpers/Cast.h>

#include <cstdio>    // _snprintf_s
#include <vector>

#include "../../SW/BeaconPlacement.inc"

namespace
{
    // =========================================================================
    // TARGET SCATTER
    //
    // Antares' SW.AITargeting picks an EXACT cell -- `Base` is the base centre,
    // every launch, forever. AITargeting.RandomX/Y jitter that by a few cells
    // so a superweapon that delivers something somewhere stops delivering it to
    // the same tile every time. See SW/TargetScatter.h for the core and for the
    // determinism rules.
    // =========================================================================

    // Which side of the configuration a launch uses.
    //
    // ⚠ Decided by the OWNING HOUSE, and nothing else. That is synced state, so
    // every client picks the same side -- which matters because the side picks
    // the band, and the band drives the number of values drawn from the shared
    // generator. An earlier version tried to detect whether Antares' AI
    // targeting had chosen the cell; it could not, because TryFire and the
    // SpecialPlace handler both live in Antares.dll and reach Fire_SW
    // identically. House control answers the question the keys actually ask
    // ("AI shots" vs "human shots") and answers it exactly.
    const SWExt::ScatterSide& ScatterSideFor(const SWTypeExt::ExtData* pExt,
                                             HouseClass* pHouse)
    {
        return pExt->Scatter.For(pHouse->IsControlledByHuman());
    }

    // Evaluate one per-axis-gated constraint at whichever cell its axis flags
    // select. Returns true when the check is off, or on and satisfied.
    //
    // `(ax,ay)` is the aim point, `(cx,cy)` the candidate. At the target stage
    // the caller passes the aim point as both, which collapses every enabled
    // combination onto the aim point -- see PlanProbe.
    bool ProbePasses(const SWTypeExt::ExtData* pExt, HouseClass* pHouse,
                     bool enableX, bool enableY,
                     int ax, int ay, int cx, int cy,
                     bool designators, bool inhibitors)
    {
        auto const probe = SWExt::PlanProbe(enableX, enableY, ax, ay, cx, cy);
        if (!probe.Enabled)
            return true;

        const CellStruct cell{ static_cast<short>(probe.X),
                               static_cast<short>(probe.Y) };

        // A probe point is the aim point's coordinate on any axis that is not
        // displaced and a clamped candidate coordinate on any that is, so it is
        // always in bounds -- but an in-bounds cell can still be illegal, and a
        // check cannot be satisfied at a cell that does not exist.
        if (!MapClass::Instance.CoordinatesLegal(cell))
            return false;

        return pExt->AllowsFireAt(pHouse, cell, designators, inhibitors);
    }

    // Stage 1: may this superweapon be aimed here at all?
    //
    // Checked against the cell TARGETING chose, before any dice. A failure here
    // refuses the launch: the shot was pointed somewhere it may not go, and no
    // amount of scattering makes that legitimate.
    bool PassesTargetStage(const SWTypeExt::ExtData* pExt, HouseClass* pHouse,
                           const SWExt::ScatterChecks& checks,
                           const CellStruct& aim)
    {
        if (!checks.AnyTarget())
            return true;

        return ProbePasses(pExt, pHouse,
                           checks.X.TargetRequireDesignator,
                           checks.Y.TargetRequireDesignator,
                           aim.X, aim.Y, aim.X, aim.Y, true, false)
            && ProbePasses(pExt, pHouse,
                           checks.X.TargetAvoidInhibitors,
                           checks.Y.TargetAvoidInhibitors,
                           aim.X, aim.Y, aim.X, aim.Y, false, true);
    }

    // Move the target cell, in place, before anything downstream reads it.
    // Returns false when the launch should be refused outright.
    //
    // ⚠ ScenarioClass::Instance->Random, never Randomizer::Global -- YRpp's own
    // header says the global one is for the map generator and other randomness
    // OUTSIDE a match. It is not replicated.
    bool ApplyTargetScatter(const SWTypeExt::ExtData* pExt, HouseClass* pHouse,
                            SuperWeaponTypeClass* pType, CellStruct& cell)
    {
        auto const& side = ScatterSideFor(pExt, pHouse);
        const bool human = pHouse->IsControlledByHuman();
        const char* who  = human ? "human" : "AI";

        if (!PassesTargetStage(pExt, pHouse, side.Checks, cell))
        {
            Debug::Log("[SuperWeaponExt] %s house %d (%s): aim point (%d,%d) "
                       "fails the target-stage constraints; refused\n",
                       pType->ID, pHouse->ArrayIndex, who, cell.X, cell.Y);
            return false;
        }

        if (!side.Band.Active())
            return true;

        auto const& bounds = MapClass::Instance.MapCoordBounds;
        const SWExt::ScatterBounds limits{
            bounds.Left, bounds.Top, bounds.Right, bounds.Bottom };

        auto& random = ScenarioClass::Instance->Random;

        auto const roll = [&random](int lo, int hi)
        {
            return random.RandomRanged(lo, hi);
        };

        const int aimX = cell.X;
        const int aimY = cell.Y;

        // A candidate must be a real cell AND satisfy whatever the random stage
        // requires -- with designators and inhibitors planning their probes
        // SEPARATELY, because their axis flags are independent. One evaluation
        // can therefore test two different cells.
        //
        // Both halves are synced, so every client accepts and rejects the same
        // candidates and therefore makes the same number of draws.
        auto const accept = [&](int cx, int cy)
        {
            const CellStruct candidate{ static_cast<short>(cx),
                                        static_cast<short>(cy) };

            if (!MapClass::Instance.CoordinatesLegal(candidate))
                return false;

            auto const& c = side.Checks;

            return ProbePasses(pExt, pHouse,
                               c.X.RandomRequireDesignator,
                               c.Y.RandomRequireDesignator,
                               aimX, aimY, cx, cy, true, false)
                && ProbePasses(pExt, pHouse,
                               c.X.RandomAvoidInhibitors,
                               c.Y.RandomAvoidInhibitors,
                               aimX, aimY, cx, cy, false, true);
        };

        auto const out = SWExt::ResolveScatter(side.Band, aimX, aimY,
                                               limits, roll, accept);

        if (!out.Fire)
        {
            Debug::Log("[SuperWeaponExt] %s house %d (%s): no cell within the "
                       "scatter band passed the random-stage constraints after "
                       "%d tries, and the aim point (%d,%d) does not either; "
                       "refused\n",
                       pType->ID, pHouse->ArrayIndex, who, out.Attempts,
                       aimX, aimY);
            return false;
        }

        if (out.UsedOriginal)
        {
            Debug::Log("[SuperWeaponExt] %s house %d (%s): no scattered cell "
                       "passed after %d tries; firing at the aim point (%d,%d)\n",
                       pType->ID, pHouse->ArrayIndex, who, out.Attempts,
                       aimX, aimY);
            return true;
        }

        if (!out.Shifted)
            return true;

        Debug::Log("[SuperWeaponExt] %s house %d (%s): target (%d,%d) scattered "
                   "to (%d,%d) on try %d%s\n",
                   pType->ID, pHouse->ArrayIndex, who, aimX, aimY,
                   out.X, out.Y, out.Attempts,
                   out.Clamped ? " [clamped to map]" : "");

        cell.X = static_cast<short>(out.X);
        cell.Y = static_cast<short>(out.Y);
        return true;
    }

    // Denial-log throttle: one line per (house, superweapon) per ~30s.
    //
    // A broke AI retries a superweapon it cannot afford on almost every pass.
    // Measured: 314 denials from one house in a few minutes of play, and that
    // grows with match length. The information is worth keeping, the repetition
    // is not — so suppressed attempts are COUNTED and reported on the next line
    // rather than discarded, which keeps "how often is this happening" readable.
    //
    // Diagnostic only: it changes no verdict, and Debug::Log output is not part
    // of simulation state, so throttling cannot affect lockstep.
    constexpr int DenialLogInterval = 450;   // ~30s at 15fps

    struct DenialRecord
    {
        int House;
        int SW;
        int LastFrame;
        int Suppressed;
    };

    // Bounded in practice by houses x superweapons; both are small.
    std::vector<DenialRecord> g_denialLog;

    // Returns true if this denial should be printed. `suppressed` receives how
    // many were swallowed since the last printed one.
    bool ShouldLogDenial(int houseIdx, int swIdx, int frame, int& suppressed)
    {
        suppressed = 0;

        for (auto& e : g_denialLog)
        {
            if (e.House != houseIdx || e.SW != swIdx)
                continue;

            // A new scenario restarts the frame counter; treat any backwards
            // jump as a fresh match rather than suppressing for 30s of it.
            if (frame < e.LastFrame || frame - e.LastFrame >= DenialLogInterval)
            {
                suppressed    = e.Suppressed;
                e.Suppressed  = 0;
                e.LastFrame   = frame;
                return true;
            }

            ++e.Suppressed;
            return false;
        }

        g_denialLog.push_back(DenialRecord{ houseIdx, swIdx, frame, 0 });
        return true;
    }
}

DEFINE_HOOK(0x4FAE50, HouseClass_Fire_SW_ConstraintVeto, 0x7)
{
    enum { Continue = 0, Deny = 0x4FAEF3 };

    GET(HouseClass*, pThis, ECX);
    GET_STACK(int, idxSW, 0x4);
    GET_STACK(CellStruct*, pCoords, 0x8);

    if (!pThis || !pCoords)
        return Continue;

    SuperClass* const pSuper = pThis->Supers.GetItemOrDefault(idxSW);
    if (!pSuper || !pSuper->Type)
        return Continue;

    auto const pExt = SWTypeExt::ExtMap.Find(pSuper->Type);

    // Not our superweapon to police — leave it entirely alone. This early-out
    // is what keeps a mod that never touches SWExt.* tags at zero added cost.
    if (!pExt || (!pExt->IsConfigured()
                  && !pExt->ParaDrop.Enabled
                  && !pExt->KeepSelectedAfterFire
                  && !pExt->Money.Active()
                  && !pExt->Scatter.Active()))
    {
        return Continue;
    }

    // SCATTER FIRST, before anything else reads the cell.
    //
    // Mutating *pCoords rather than a copy is deliberate: everything downstream
    // -- the inhibitor veto, our owned paradrop, beacon placement and the
    // engine's own launch -- takes the cell from this pointer, and they must all
    // agree on where the shot actually lands. The caller's storage is dead after
    // this call at all 17 call sites (each picks a cell, fires, discards it),
    // and every client performs the identical mutation.
    //
    // ⚠ The scatter OWNS the constraint checking for a superweapon that
    // declares a band, because it is the thing that knows there are two cells
    // worth checking. The generic check below is skipped in that case rather
    // than run a third time against the final cell -- running both would make
    // AITargeting.RandomAvoidInhibitors=no a lie.
    //
    // A superweapon with no band takes neither branch's new behaviour: the
    // AITargeting.* stage keys do nothing without a band, so a mod that never
    // sets one sees exactly the old path.
    const bool scattering = pExt->Scatter.Active();

    if (scattering)
    {
        if (!ApplyTargetScatter(pExt, pThis, pSuper->Type, *pCoords))
        {
            R->AL(0);
            return Deny;
        }
    }

    if (!scattering && pExt->IsConfigured() && !pExt->AllowsFireAt(pThis, *pCoords))
    {
        int suppressed = 0;
        if (ShouldLogDenial(pThis->ArrayIndex, idxSW, Unsorted::CurrentFrame, suppressed))
        {
            // ⚠ ONE Debug::Log call per line. Debug::Log prepends "[Phobos] "
            // to EVERY call, so splitting a line across calls injects the
            // prefix mid-sentence — which is exactly what the first version of
            // this throttle produced in game.
            char suffix[64] = {};
            if (suppressed)
            {
                _snprintf_s(suffix, sizeof(suffix),
                            " (+%d attempt(s) since last line)", suppressed);
            }

            Debug::Log("[SuperWeaponExt] denied %s for house %d at (%d,%d): "
                       "inhibitor/designator constraints not met%s\n",
                       pSuper->Type->ID, pThis->ArrayIndex,
                       pCoords->X, pCoords->Y, suffix);

            // Explain WHICH inhibitor blocked and how its radius was arrived at.
            // Only on a real refused launch, never the per-frame cursor path.
            pExt->LogDenial(pThis, *pCoords);
        }

        R->AL(0);
        return Deny;
    }

    // =========================================================================
    // FINANCIAL REQUIREMENTS
    //
    // The AI fires superweapons without regard to its bank balance. Antares'
    // Money.Amount does not help: it calls TransactMoney UNGUARDED at launch, so
    // a costly superweapon fires anyway and can push the house negative. This
    // gate is the missing affordability half, and it applies to the AI paths for
    // free because they all funnel through Fire_SW.
    //
    // Desync-safe by position, not by care: house credits are synced simulation
    // state and this hook is downstream of the event queue, so every client
    // evaluates the same balance on the same frame.
    //
    // IsControlledByHuman(), NOT IsControlledByCurrentPlayer() — the latter
    // answers "can the human at THIS machine drive it", which differs per client.
    // =========================================================================
    const bool firerIsHuman = pThis->IsControlledByHuman();

    if (pExt->Money.Active())
    {
        const int money = pThis->Available_Money();

        if (!pExt->Money.Allows(money, firerIsHuman))
        {
            int suppressed = 0;
            if (ShouldLogDenial(pThis->ArrayIndex, idxSW, Unsorted::CurrentFrame,
                                suppressed))
            {
                const auto spec = pExt->Money.Resolve(firerIsHuman);

                char suffix[64] = {};
                if (suppressed)
                {
                    _snprintf_s(suffix, sizeof(suffix),
                                " (+%d attempt(s) since last line)", suppressed);
                }

                Debug::Log("[SuperWeaponExt] denied %s for house %d: credits %d "
                           "fails cost %d / min %d / max %d (%s)%s\n",
                           pSuper->Type->ID, pThis->ArrayIndex, money,
                           spec.Cost, spec.Min, spec.Max,
                           firerIsHuman ? "human" : "AI", suffix);
            }

            R->AL(0);
            return Deny;
        }

        // Also log the PASS. Without it, "no denials" is ambiguous between "the
        // gate held and nobody tripped it" and "the gate never ran" — which is
        // exactly the question an AI-side rule raises, since an AI that is simply
        // rich looks identical to one that is ungated.
        Debug::Log("[SuperWeaponExt] %s allowed for house %d (%s, %s): credits %d "
                   "passes cost %d / min %d / max %d\n",
                   pSuper->Type->ID, pThis->ArrayIndex,
                   pThis->Type ? pThis->Type->ID : "?",
                   firerIsHuman ? "human" : "AI", money,
                   pExt->Money.Resolve(firerIsHuman).Cost,
                   pExt->Money.Resolve(firerIsHuman).Min,
                   pExt->Money.Resolve(firerIsHuman).Max);
    }

    // OWNED PARADROP.
    //
    // ⚠ Why here and not at SuperClass::Launch (0x6CC390), which would be the
    // natural seat: Antares hooks 0x6CC390 and dispatches its own SW_ParaDrop
    // from there — and so do Ares and Phobos, three consumers on one address.
    // Syringe would chain us behind Antares, which returns "handled" for a
    // ParaDrop type, so our handler would never run. Injection order decides it,
    // which is not something to build on.
    //
    // Fire_SW's entry is upstream of all of that, so aborting here is the only
    // way to guarantee Antares' copy does not also fire.
    //
    // Sits AFTER the constraint check on purpose: an owned paradrop still obeys
    // inhibitors and designators.
    // ⚠ READINESS GATE. This hook is at Fire_SW's ENTRY, upstream of every
    // charge check the engine performs — those live inside ClickFire, which the
    // abort below skips. Without this, each click ran the drop again regardless
    // of the recharge timer: the timer reset correctly and the superweapon was
    // still spammable. Found in-game; CanFire is the engine's own predicate and
    // the same one Phobos gates its sidebar launch on.
    //
    // Returning Continue rather than Deny lets the engine give its normal
    // not-ready response (the "not charged" EVA line) instead of silently
    // swallowing the click.
    if (pExt->ParaDrop.Enabled && !pSuper->CanFire())
        return Continue;

    // ⚠ CHARGE LAST, and only for a shot that is actually going off.
    //
    // The readiness gate above is deliberately re-tested here for NON-paradrop
    // superweapons too. Without it a click on a recharging superweapon would be
    // billed: this hook sits at Fire_SW's entry, upstream of every charge check
    // the engine performs, so "not ready" is not visible to us until we ask.
    // Both branches below end up returning Continue in that case, so adding the
    // test changes nothing except who pays.
    //
    // Deducted here rather than in either branch so the owned-paradrop path and
    // the engine path bill identically, exactly once.
    if (pExt->Money.Active() && pSuper->CanFire())
    {
        const int cost = pExt->Money.CostFor(firerIsHuman);
        if (cost != 0)
        {
            // Negative cost is income, which is why this is TransactMoney(-cost)
            // rather than a subtraction guarded on cost > 0.
            pThis->TransactMoney(-cost);

            Debug::Log("[SuperWeaponExt] %s charged house %d %d credits "
                       "(%d remaining)\n", pSuper->Type->ID, pThis->ArrayIndex,
                       cost, pThis->Available_Money());
        }
    }

    // Record where this launch landed, for units whose standing order is
    // "converge on the last superweapon impact".
    //
    // Placed AFTER the constraint veto and the readiness gate so a refused or
    // not-yet-charged click does not move anybody, but BEFORE the owned-paradrop
    // dispatch so both the owned path and the engine path record identically.
    // Fire_SW runs on every client, so this write is synced by position.
    SWExt::StandingOrders::RecordImpact(pThis, *pCoords);

    // Auto-fire: note that this launch happened, for rules watching it. Recorded
    // rather than acted on, so nothing fires from inside another launch.
    SWTypeExt::RecordLaunch(pThis, idxSW, *pCoords);

    // Beacon placement. After the veto and readiness gates, so a refused or
    // uncharged click places nothing.
    PlaceBeacon(pExt, pThis, *pCoords, idxSW);

    if (pExt->ParaDrop.Enabled
        && SWTypeExt::RunOwnedParaDrop(pSuper->Type, pThis, *pCoords))
    {
        // Aborting skips SuperClass::ClickFire, which is what would normally
        // spend the charge — so without this the superweapon would stay ready
        // and could be fired every frame. Reset() is the engine's own "put this
        // back on the clock" (the same one the spy-infiltration trigger action
        // uses), so the recharge behaves as the modder configured it.
        pSuper->Reset();

        // Aborting also skips the DESELECT. Unsorted::CurrentSWType (0x8809A0,
        // == DisplayClass::Instance.CurrentSWTypeIndex) is what keeps the cursor
        // armed, and the engine clears it from inside SuperClass::Launch — at
        // eleven separate `movl $-1` sites, one per superweapon action branch.
        // We never reach any of them, so without this the cursor stays holding
        // the superweapon after it has already fired.
        //
        // ⚠ Guarded on CurrentPlayer. Fire_SW runs on EVERY client (it is
        // downstream of the event queue), so clearing unconditionally would
        // reset the local cursor whenever a *remote* player fired. The pending-SW
        // index is local UI state, so touching it per-client is desync-safe —
        // but only if we touch the right client's.
        // SWExt.KeepSelectedAfterFire=yes simply skips this, leaving the cursor
        // armed so a fast-recharging superweapon can be fired again without
        // going back to the cameo.
        if (pThis == HouseClass::CurrentPlayer && !pExt->KeepSelectedAfterFire)
            DisplayClass::Instance.CurrentSWTypeIndex = -1;

        R->AL(1);      // report "it fired"
        return Deny;   // "Deny" here means "skip the engine's own launch"
    }

    // For every OTHER superweapon we let the engine launch normally, so the
    // engine also performs its own deselect — the eleven `movl $-1` sites inside
    // SuperClass::Launch. Rather than suppress those (Antares owns 0x6CC390, so
    // we cannot), re-arm the cursor on the next frame.
    //
    // Deliberately a ONE-SHOT request: re-arming every frame would fight the
    // player right-clicking to put the superweapon away.
    if (pExt->KeepSelectedAfterFire && pThis == HouseClass::CurrentPlayer)
        SWTypeExt::RequestKeepSelected(idxSW);

    return Continue;
}

// STANDING ORDERS: the per-house "last SW impact cell" blackboard is written
// from this hook, because it is the one place every launch is guaranteed to
// pass. See src/Ext/Techno/StandingOrders.h.
//
// On the earlier save/load concern: the blackboard is NOT serialized, and that
// turns out to be safe rather than merely unfinished. Every client reloads with
// it empty, so all clients agree; SWImpact-mode units then read "no
// destination" and are simply left alone until the next launch. The cost is a
// behavioural gap across a reload, not a divergence.

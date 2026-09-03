/*
 * SuperWeaponExt — unit standing orders.
 *
 * ============================================================================
 * WHY THIS FILE IS HELD TO A STRICTER STANDARD THAN THE REST
 * ============================================================================
 *
 * Inhibitors, cursors and hotkeys only ever REFUSE things or touch local UI
 * state, so a client that disagrees just shows a different mouse pointer. This
 * one ISSUES ORDERS — it mutates simulation state that the lockstep model
 * requires every client to agree on, exactly like the engine's own AI.
 *
 * That is allowed, under one rule: the decision must be a pure function of
 * SYNCED state on a SYNCED frame. Nothing in this file may read
 *
 *   - HouseClass::CurrentPlayer  (differs per client)
 *   - the mouse or the tactical view
 *   - wall-clock time, render frames, or unsynced RNG
 *
 * It reads Unsorted::CurrentFrame, TechnoClass::Array and object coordinates,
 * all identical across clients at the moment this runs. Ties are broken by array
 * order rather than "whoever we happened to find first" for the same reason —
 * see PickNearest in SW/StandingOrder.h.
 *
 * ============================================================================
 * COST
 * ============================================================================
 *
 * Three guards keep this off the profile of a mod that does not use it:
 *
 *   1. A cached "does any loaded type even have an order?" flag — no order
 *      configured means one bool test per frame and nothing else.
 *   2. Candidates are gathered ONCE per tick and shared by every ordered unit,
 *      the same shape as the ratio counting in Constraint.h.
 *   3. Units are staggered across their interval by array index, so a thousand
 *      ordered units never all re-evaluate on one frame.
 */
#include "StandingOrders.h"

#include <Ext/TechnoType/Body.h>

#include <SW/StandingOrder.h>

#include <FootClass.h>
#include <GeneralDefinitions.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Utilities/Debug.h>

#include <vector>

namespace
{
    // -------------------------------------------------------------------------
    // Per-house "last superweapon impact" blackboard.
    //
    // Written from the Fire_SW hook, which every launch passes through, so it is
    // populated identically on every client.
    //
    // Sized from HouseClass::Array rather than a hardcoded 8 or 32: house counts
    // above the vanilla limit are a thing this ecosystem actually does, and a
    // fixed bound here would be a silent out-of-bounds write on those setups.
    //
    // NOT serialized. A save/load loses the recorded cells, after which
    // SWImpact-mode units simply have nowhere to head until the next launch.
    // Every client loses the same entries, so this cannot desync — it is a
    // behavioural gap, not a correctness one, and it is why the feature reads
    // the blackboard as "no destination" instead of "stop".
    // -------------------------------------------------------------------------
    struct ImpactRecord
    {
        CellStruct Cell  {};
        bool       Valid = false;
    };

    std::vector<ImpactRecord> g_lastImpact;

    // A candidate destination in engine terms, gathered once per tick. Relation
    // is NOT stored: it depends on who is asking, so it is derived per unit.
    struct Candidate
    {
        int         TypeIndex;
        HouseClass* Owner;
        int         CellX;
        int         CellY;
    };

    // "Idle" = the player or AI has not given it something to do. Deliberately
    // conservative: a standing order must never countermand a real order.
    bool IsIdle(FootClass* pFoot)
    {
        return !pFoot->Destination && !pFoot->Target;
    }

    bool IsUsable(TechnoClass* pTechno)
    {
        return pTechno
            && pTechno->IsAlive
            && pTechno->Health > 0
            && !pTechno->InLimbo;
    }

    // Does ANY loaded TechnoType configure a standing order? Re-checked
    // periodically rather than every frame; rules do not change mid-match, but a
    // scenario reload swaps the type array wholesale.
    bool AnyOrdersConfigured()
    {
        for (int i = 0; i < TechnoTypeClass::Array.Count; ++i)
        {
            auto const pType = TechnoTypeClass::Array.GetItem(i);
            if (!pType)
                continue;

            auto const pExt = TechnoTypeExt::ExtMap.Find(pType);
            if (pExt && pExt->Order.Active())
                return true;
        }
        return false;
    }

    void GatherCandidates(std::vector<Candidate>& out)
    {
        out.clear();
        out.reserve(64);

        for (int i = 0; i < TechnoClass::Array.Count; ++i)
        {
            TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
            if (!IsUsable(pTechno))
                continue;

            auto const pType = pTechno->GetTechnoType();
            if (!pType)
                continue;

            const auto cell = CellClass::Coord2Cell(pTechno->GetCoords());

            out.push_back(Candidate{
                pType->GetArrayIndex(),
                pTechno->Owner,
                cell.X,
                cell.Y });
        }
    }

    // Reduce the shared candidate list to what THIS unit's order accepts,
    // filling in the relation relative to its owner. Preserves candidate order,
    // which is TechnoClass::Array order — the property PickNearest's tie-break
    // relies on for determinism.
    void ScopeForUnit(const SWExt::StandingOrder& order, HouseClass* pOwner,
                      const std::vector<Candidate>& candidates,
                      std::vector<SWExt::OrderTarget>& out)
    {
        out.clear();

        for (auto const& c : candidates)
        {
            if (!order.CoversType(c.TypeIndex))
                continue;

            SWExt::OrderTarget t;
            t.TypeIndex = c.TypeIndex;
            t.CellX     = c.CellX;
            t.CellY     = c.CellY;
            t.Active    = true;

            if (c.Owner == pOwner)
                t.Rel = SWExt::Relation::Owner;
            else if (pOwner->IsAlliedWith(c.Owner))
                t.Rel = SWExt::Relation::Allies;
            else
                t.Rel = SWExt::Relation::Enemies;

            out.push_back(t);
        }
    }
}

void SWExt::StandingOrders::RecordImpact(HouseClass* pHouse, const CellStruct& cell)
{
    if (!pHouse)
        return;

    const int idx = pHouse->ArrayIndex;
    if (idx < 0)
        return;

    if (static_cast<int>(g_lastImpact.size()) <= idx)
        g_lastImpact.resize(idx + 1);

    g_lastImpact[idx].Cell  = cell;
    g_lastImpact[idx].Valid = true;
}

void SWExt::StandingOrders::ClearImpacts()
{
    g_lastImpact.clear();
}

void SWExt::StandingOrders::Tick()
{
    // Re-check roughly once a minute (15 fps => 900 frames). Cheap enough that
    // the periodic walk is invisible, current enough that a scenario switch is
    // picked up long before it matters.
    static bool s_anyOrders   = false;
    static bool s_everChecked = false;
    static int  s_lastFrame   = -1;

    const int frame = Unsorted::CurrentFrame;

    // A new scenario restarts the frame counter, so a frame that did not advance
    // forward means we are in a different match than the one that filled the
    // blackboard. Detecting it this way costs one comparison and, more to the
    // point, needs no new hook address — the alternative was guessing at a
    // scenario-start seat that the encyclopedia does not yet document.
    //
    // CurrentFrame is synced, so every client notices the reset on the same tick.
    if (frame < s_lastFrame)
    {
        ClearImpacts();
        s_everChecked = false;
    }
    s_lastFrame = frame;

    if (!s_everChecked || (frame % 900) == 0)
    {
        s_everChecked = true;
        s_anyOrders   = AnyOrdersConfigured();
    }

    if (!s_anyOrders)
        return;

    // Reused across ticks so the steady state does no allocation at all.
    static std::vector<Candidate> s_candidates;
    static std::vector<SWExt::OrderTarget> s_scoped;

    bool gathered = false;

    for (int i = 0; i < TechnoClass::Array.Count; ++i)
    {
        TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
        if (!IsUsable(pTechno) || pTechno->Deactivated)
            continue;

        // Buildings cannot be ordered anywhere; only FootClass moves.
        auto const pFoot = abstract_cast<FootClass*>(pTechno);
        if (!pFoot)
            continue;

        auto const pType = pTechno->GetTechnoType();
        if (!pType)
            continue;

        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        if (!pTypeExt)
            continue;

        const SWExt::StandingOrder& order = pTypeExt->Order;
        if (!order.Active())
            continue;

        // Stagger before any other work — this is the guard that keeps the cost
        // linear when a mod gives orders to a common unit.
        if (!SWExt::DueThisFrame(order, frame, i))
            continue;

        if (order.IdleOnly && !IsIdle(pFoot))
            continue;

        HouseClass* const pOwner = pTechno->Owner;
        if (!pOwner)
            continue;

        CellStruct destination{};
        bool haveDestination = false;

        if (order.Mode == SWExt::StandingOrderMode::SWImpact)
        {
            const int idx = pOwner->ArrayIndex;
            if (idx >= 0 && idx < static_cast<int>(g_lastImpact.size())
                && g_lastImpact[idx].Valid)
            {
                destination     = g_lastImpact[idx].Cell;
                haveDestination = true;
            }
        }
        else
        {
            if (!gathered)
            {
                GatherCandidates(s_candidates);
                gathered = true;
            }

            ScopeForUnit(order, pOwner, s_candidates, s_scoped);

            const auto unitCell = CellClass::Coord2Cell(pTechno->GetCoords());
            if (auto const* pPick =
                    SWExt::PickNearest(order, s_scoped, unitCell.X, unitCell.Y))
            {
                destination.X   = static_cast<short>(pPick->CellX);
                destination.Y   = static_cast<short>(pPick->CellY);
                haveDestination = true;
            }
        }

        if (!haveDestination)
            continue;

        // Already standing there — re-issuing would jitter the unit in place.
        const auto unitCell = CellClass::Coord2Cell(pTechno->GetCoords());
        if (unitCell.X == destination.X && unitCell.Y == destination.Y)
            continue;

        CellClass* const pDestCell = MapClass::Instance.TryGetCellAt(destination);
        if (!pDestCell)
            continue;

        // Queue the mission FIRST, then set the destination. That order is the
        // engine's own idiom for a scripted move, confirmed independently
        // against Phobos (Ext/Script/Mission.Move.cpp:152-156) and Antares
        // (Ext/Building/Hooks.Tunnels.cpp:225-226). Reversing it leaves the
        // destination to be cleared by the mission change.
        //
        // Both are RX/R0 stubs in YRpp, but these are VIRTUAL calls, so dispatch
        // reaches the real game implementation. A qualified call would silently
        // no-op — the trap recorded in yrpp-r0-stub-vs-jmpthis.
        pFoot->QueueMission(Mission::Move, false);
        pFoot->SetDestination(pDestCell, true);
    }
}

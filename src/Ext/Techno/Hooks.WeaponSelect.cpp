/*
 * SuperWeaponExt — "use a different weapon against a designator/inhibitor".
 *
 * ============================================================================
 * WHY A VTABLE WRAP AND NOT A CODE HOOK
 * ============================================================================
 *
 * TechnoClass::WhatWeaponShouldIUse (`0x6F3330`, YRpp calls it SelectWeapon) is
 * the single most contested function in the game. Inside its body live NINE
 * Phobos hooks (Interceptor, MultiWeapon, ForceFire, ForceWeapon, Gattling,
 * Airstrike, IsLocomotor, the main one, AntiAir) and TWO Antares ones
 * (NoAmmoWeapon, Verses).
 *
 * Two tempting approaches are both wrong:
 *
 *   1. Hook the ENTRY and short-circuit to the epilogue. This is what Antares'
 *      (commented-out) TechnoClass_SelectWeapon did, and it would bypass all
 *      eleven of those hooks — silently breaking Gattling, MultiWeapon, ammo
 *      handling and Verses. It is also broken on its own terms: it jumped to
 *      `0x6F3813`, which disassembles as a bare `ret 0x4` reached only AFTER
 *      four pops and an `add esp,8` at `0x6F3807`–`0x6F3810`. Jumping there from
 *      the entry, before anything is pushed, returns on a corrupted stack.
 *
 *   2. Hook a `ret`. The function has several return paths, so this needs one
 *      hook per exit and still lands inside a heavily-hooked body.
 *
 * SelectWeapon is VIRTUAL (`YRpp/TechnoClass.h:227`). Replacing the vtable slot
 * gives a wrapper that CALLS the original, so every framework hook inside still
 * runs and decides first, and we only adjust the answer afterwards. Same
 * architecture as the Layer 3 cursor wrapper in Hooks.Cursor.cpp.
 *
 * ============================================================================
 * ⚠ SIX SLOTS, THREE DIFFERENT ORIGINALS — the bug this file shipped with
 * ============================================================================
 *
 * The first version of this file searched the binary for references to
 * `0x6F3330`, found exactly four .rdata vtable slots, and concluded "one per
 * TechnoClass subclass". That reasoning is INVALID, and the conclusion was
 * wrong: UnitClass and InfantryClass OVERRIDE SelectWeapon, so their slots do
 * not contain `0x6F3330` and a search for references to it structurally cannot
 * find them. The count of four was read as confirmation of coverage when it was
 * really just the number of classes that DON'T override.
 *
 * Dumped straight out of gamemd-spawn.exe (slot = AbsVTable + 0x2E4):
 *
 *   class         AbsVTable    slot        holds
 *   Aircraft      0x7E22A4     0x7E2588    0x6F3330   TechnoClass::SelectWeapon
 *   Building      0x7E3EBC     0x7E41A0    0x6F3330
 *   FootClass     0x7E8C94     0x7E8F78    0x6F3330   (abstract: no live object)
 *   TechnoClass   0x7F4960     0x7F4C44    0x6F3330   (abstract: no live object)
 *   Infantry      0x7EB058     0x7EB33C    0x5218E0   InfantryClass::SelectWeapon
 *   Unit          0x7F5C70     0x7F5F54    0x746CD0   UnitClass::SelectWeapon
 *
 * So the shipped wrapper was live for AIRCRAFT AND BUILDINGS ONLY — the other
 * two patched slots belong to abstract bases nothing instantiates. Every rule
 * configured on a vehicle or an infantryman silently did nothing, which is
 * exactly why no log in any test game ever contained a `weapon N -> M` line.
 * Found by the PerfExt session, 2026-11-11, and confirmed against the binary.
 *
 * ⚠ Each slot must call ITS OWN original. Wrapping the Unit slot but calling
 * `0x6F3330` would skip UnitClass::SelectWeapon entirely — so the three thin
 * wrappers below differ only in which address they call.
 *
 * All six slots are unclaimed: no Phobos or Antares source references them and
 * the hook registry has no rows for them.
 *
 * ============================================================================
 * DETERMINISM
 * ============================================================================
 *
 * Weapon choice feeds combat, which is synced, and this wrapper also runs on
 * PerfExt's worker threads during look-ahead target checks. It is therefore
 * STRICTLY READ-ONLY: every table it consults is built on the game thread from
 * the shared frame tick (`SWExt::WeaponSelect::FrameTick`), and the wrapper
 * itself allocates nothing and mutates nothing. The one exception is the
 * diagnostic log, whose fast path is a lock-free relaxed read and which is not
 * game state.
 *
 * ⚠ That includes `TechnoTypeExt::UnifiedIndex`, which looks like a pure
 * accessor and is not — it caches into the type's ext on first call. FrameTick
 * warms it for every type so no worker ever takes that path.
 *
 * ⚠ WHY THE SNAPSHOT MUST BE BUILT FROM THE TICK AND NOT ON FIRST USE.
 * The WhileNear rules need the POSITIONS of other objects, not just type data.
 * The first version built that list lazily, on the first weapon choice of each
 * frame. The list contents are synced, but the INSTANT it was sampled was not:
 * weapon choice also runs from cursor drawing, so on a client whose mouse was
 * over a unit the snapshot was taken before the object loop, and on a client
 * whose mouse was elsewhere it was taken part-way through it. Same frame,
 * different unit positions, different weapon, desync. Building it once at the
 * post-loop tick makes the sampling point identical on every client.
 *
 * The old header claimed determinism on the grounds that the wrapper "reads only
 * type data and the target's identity". That was true of the Vs* rules and false
 * of the WhileNear* ones.
 */
#include <Ext/SWType/Body.h>
#include <Ext/TechnoType/Body.h>

#include <SuperWeaponTypeClass.h>
#include <CellClass.h>
#include <Fundamentals.h>
#include <InfantryClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <UnitClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <Ext/Techno/WeaponSelect.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
    // -------------------------------------------------------------------------
    // "Is this TechnoType configured as an inhibitor / designator anywhere?"
    //
    // Answered from a cache: this runs inside weapon selection, which is called
    // from targeting, cursor rendering, threat evaluation and firing, so walking
    // every superweapon per call is not an option.
    //
    // The cache is a union across all superweapons. The per-superweapon form
    // ("honor indexes") is answered directly from that SW's ext instead, since a
    // named SW needs no search.
    //
    // ⚠ Built ONLY from SWExt::WeaponSelect::FrameTick, on the game thread. The
    // wrapper may run on a PerfExt worker thread, so it must never trigger a
    // rebuild. Until the first tick has run, g_cacheReady is false and the Vs*
    // rules simply do not apply — a one-frame warm-up that is identical on every
    // client because it is keyed to the frame loop, not to wall clock.
    // -------------------------------------------------------------------------
    std::vector<int> g_inhibitorTypes;    // sorted unified type indices
    std::vector<int> g_designatorTypes;
    int  g_builtForCount = -1;            // SuperWeaponTypeClass::Array.Count when built
    int  g_builtForTypes = -1;            // TechnoTypeClass::Array.Count when built
    bool g_cacheReady    = false;

    void BuildLogTable(int typeCount);

    void BuildCache()
    {
        g_inhibitorTypes.clear();
        g_designatorTypes.clear();

        for (int i = 0; i < SuperWeaponTypeClass::Array.Count; ++i)
        {
            auto const pSW = SuperWeaponTypeClass::Array.GetItem(i);
            if (!pSW)
                continue;

            auto const pExt = SWTypeExt::ExtMap.Find(pSW);
            if (!pExt)
                continue;

            for (int idx : pExt->Inhibitors.TypeIndices)
                g_inhibitorTypes.push_back(idx);

            for (int idx : pExt->Designators.TypeIndices)
                g_designatorTypes.push_back(idx);
        }

        // Sorted + deduplicated so lookups are a binary search.
        for (auto* v : { &g_inhibitorTypes, &g_designatorTypes })
        {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }

        // ⚠ Warm TechnoTypeExt::UnifiedIndex for EVERY type, here on the game
        // thread. It caches into the type's ext on first use, so leaving it cold
        // would mean a worker thread doing the first call — a write on a path
        // that must be read-only — and, worse, every cold call runs
        // TechnoTypeClass::Array.FindItemIndex, a linear scan, once per
        // candidate. Warming it makes every later call a pointer read.
        const int typeCount = TechnoTypeClass::Array.Count;
        for (int i = 0; i < typeCount; ++i)
            TechnoTypeExt::UnifiedIndex(TechnoTypeClass::Array.GetItem(i));

        BuildLogTable(typeCount);

        g_builtForCount  = SuperWeaponTypeClass::Array.Count;
        g_builtForTypes  = typeCount;
        g_cacheReady     = true;

        Debug::Log("[SuperWeaponExt] weapon-select cache: %u inhibitor type(s), "
                   "%u designator type(s)\n",
                   static_cast<unsigned>(g_inhibitorTypes.size()),
                   static_cast<unsigned>(g_designatorTypes.size()));
    }

    bool InSorted(const std::vector<int>& v, int value)
    {
        return std::binary_search(v.begin(), v.end(), value);
    }

    // Does `pTargetType` act as the given role, optionally only for one named SW?
    bool ActsAs(TechnoTypeClass* pTargetType, bool wantInhibitor,
                SuperWeaponTypeClass* pScopedSW)
    {
        if (!pTargetType)
            return false;

        const int typeIndex = TechnoTypeExt::UnifiedIndex(pTargetType);
        if (typeIndex < 0)
            return false;

        // "Honor indexes": scoped to one superweapon's list rather than the union.
        if (pScopedSW)
        {
            auto const pSWExt = SWTypeExt::ExtMap.Find(pScopedSW);
            if (!pSWExt)
                return false;

            const auto& rule = wantInhibitor ? pSWExt->Inhibitors : pSWExt->Designators;
            return rule.CoversType(typeIndex);
        }

        if (!g_cacheReady)
            return false;

        return InSorted(wantInhibitor ? g_inhibitorTypes : g_designatorTypes, typeIndex);
    }

    // -------------------------------------------------------------------------
    // "Is this techno standing inside an inhibitor's / designator's radius?"
    //
    // Built ONCE PER FRAME from SWExt::WeaponSelect::FrameTick — the post-loop
    // seat in LogicClass::AI — and never from the wrapper. Weapon selection is
    // called from targeting, cursor rendering, threat evaluation and firing, so
    // a per-call scan of the techno array would be indefensible; the per-frame
    // list is small (one entry per inhibitor/designator on the map), so the
    // per-call test over it is cheap.
    //
    // ⚠ The sampling point is the whole reason this lives in the tick. See the
    // determinism note in the file header: building it at "the first weapon
    // choice of the frame" samples object positions at a locally-determined
    // instant and desyncs. Frame N's object loop reads the snapshot taken at the
    // end of frame N-1, which every client took at the same place.
    //
    // ⚠ SIMPLIFICATION, deliberate and documented: the radius used here is the
    // TechnoType's veterancy-resolved InhibitorRange / DesignatorRange only. It
    // does NOT include the per-superweapon Ranges override, growth over time or
    // the proximity ratio, because those are properties of a (superweapon,
    // source) PAIR and weapon selection has no superweapon in hand. So a growing
    // inhibitor influences the launch veto out to its grown radius but influences
    // weapon choice out to its base radius. Anything else would require inventing
    // a superweapon to evaluate against.
    // -------------------------------------------------------------------------
    struct Influence
    {
        int  CellX;
        int  CellY;
        int  Range;      // cells
        bool IsInhibitor;
        bool IsDesignator;
    };

    std::vector<Influence> g_influence;
    bool g_influenceReady = false;

    void BuildInfluence()
    {
        g_influence.clear();
        g_influenceReady = true;

        if (!g_cacheReady)
            return;

        for (int i = 0; i < TechnoClass::Array.Count; ++i)
        {
            TechnoClass* const pTechno = TechnoClass::Array.GetItem(i);
            if (!pTechno || !pTechno->IsAlive || pTechno->Health <= 0
                || pTechno->InLimbo)
            {
                continue;
            }

            auto const pType = pTechno->GetTechnoType();
            if (!pType)
                continue;

            const int typeIndex = TechnoTypeExt::UnifiedIndex(pType);
            const bool inhib = InSorted(g_inhibitorTypes, typeIndex);
            const bool desig = InSorted(g_designatorTypes, typeIndex);
            if (!inhib && !desig)
                continue;

            auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
            if (!pTypeExt)
                continue;

            const auto rank = pTechno->Veterancy.IsElite()   ? SWExt::Rank::Elite
                            : pTechno->Veterancy.IsVeteran() ? SWExt::Rank::Veteran
                                                             : SWExt::Rank::Rookie;

            // Sight is the engine's fallback when nothing was configured, the
            // same default the constraint evaluator uses.
            const int sight = pType->Sight;
            const int range = inhib ? pTypeExt->InhibitorRange.Resolve(rank, sight)
                                    : pTypeExt->DesignatorRange.Resolve(rank, sight);

            if (range <= 0)
                continue;

            const auto cell = CellClass::Coord2Cell(pTechno->GetCoords());
            g_influence.push_back(Influence{ cell.X, cell.Y, range, inhib, desig });
        }
    }

    bool StandingInInfluence(TechnoClass* pTechno, bool wantInhibitor)
    {
        if (!pTechno || !g_influenceReady)
            return false;

        const auto cell = CellClass::Coord2Cell(pTechno->GetCoords());

        for (auto const& inf : g_influence)
        {
            if (wantInhibitor ? !inf.IsInhibitor : !inf.IsDesignator)
                continue;

            const std::int64_t dx = static_cast<std::int64_t>(inf.CellX) - cell.X;
            const std::int64_t dy = static_cast<std::int64_t>(inf.CellY) - cell.Y;
            const std::int64_t r  = inf.Range;

            if (dx * dx + dy * dy <= r * r)
                return true;
        }
        return false;
    }

    // One log line per (firer type, target type) pair, ever.
    //
    // Weapon selection runs from targeting, cursor rendering, threat evaluation
    // and firing, so an unthrottled line would bury the log within seconds. But
    // some diagnostic is needed: whether the override fired is NOT reliably
    // visible in game, because the engine may pick a secondary weapon on its own
    // for unrelated reasons, so "it looked different" proves nothing.
    //
    // ⚠ THE FAST PATH MUST NOT TAKE THE LOCK. The first version of this guarded
    // the whole thing with a mutex, reasoning that it "sits on the rare path —
    // reached only once a rule has actually matched". That reasoning is wrong for
    // the FIRER-side rules: StandingInInfluence tests the firer's own position
    // and never looks at the target, so while one MTNK stands in an inhibitor's
    // influence EVERY candidate of its target search matches. With PerfExt lane 3
    // that is 5 threads taking one mutex per candidate, ~7,000 candidates per
    // walk, ~150 walks per frame — one cache line ping-ponging between cores in
    // the game's hottest loop. TAExt's locomotor refcount did exactly this and
    // made every lane 4.5x slower (PerfExt lane3-plan.md §25).
    //
    // So: an atomic flag per (firer type, target type), read RELAXED with no
    // lock. After the first line for a pair, a matched candidate costs one shared
    // read of a line that stays in Shared state on every core. The lock is taken
    // only to write a line, and is held across Debug::Log so two threads cannot
    // interleave halves of one line.
    //
    // The firer dimension is COMPACTED to the types that actually carry a
    // WeaponVs rule — a deliberate configuration, so realistically a handful —
    // rather than types x types, which a large mod would make a multi-megabyte
    // allocation for a diagnostic.
    //
    // Relaxed is the right order: nothing is published through this flag. It
    // orders no other data, and no synced decision reads it. It is diagnostics.
    std::vector<int>                              g_logFirerSlot;  // unified index -> dense slot, -1 = no rule
    std::unique_ptr<std::atomic<unsigned char>[]> g_logFlags;
    std::size_t                                   g_logTypeCount = 0;
    std::mutex                                    g_logLock;

    // Called from BuildCache, i.e. the game thread, with no worker live.
    void BuildLogTable(int typeCount)
    {
        const std::size_t types = typeCount > 0 ? static_cast<std::size_t>(typeCount) : 0;

        g_logFirerSlot.assign(types, -1);

        int firers = 0;
        for (std::size_t i = 0; i < types; ++i)
        {
            auto const pType = TechnoTypeClass::Array.GetItem(static_cast<int>(i));
            auto const pExt  = pType ? TechnoTypeExt::ExtMap.Find(pType) : nullptr;

            if (pExt && pExt->WeaponVs.Active())
                g_logFirerSlot[i] = firers++;
        }

        g_logTypeCount = types;

        const std::size_t cells = static_cast<std::size_t>(firers) * types;
        g_logFlags = cells ? std::make_unique<std::atomic<unsigned char>[]>(cells) : nullptr;

        for (std::size_t i = 0; i < cells; ++i)
            g_logFlags[i].store(0, std::memory_order_relaxed);
    }

    void LogOnce(int firerIdx, int targetIdx, const char* firerId,
                 const char* targetId, const char* why, int chosen, int override_)
    {
        if (!g_logFlags || firerIdx < 0 || targetIdx < 0
            || static_cast<std::size_t>(firerIdx)  >= g_logFirerSlot.size()
            || static_cast<std::size_t>(targetIdx) >= g_logTypeCount)
        {
            return;
        }

        const int slot = g_logFirerSlot[static_cast<std::size_t>(firerIdx)];
        if (slot < 0)
            return;   // firer carries no WeaponVs rule; cannot reach here anyway

        auto& flag = g_logFlags[static_cast<std::size_t>(slot) * g_logTypeCount
                                + static_cast<std::size_t>(targetIdx)];

        if (flag.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> guard(g_logLock);

        if (flag.load(std::memory_order_relaxed))
            return;

        Debug::Log("[SuperWeaponExt] %s vs %s (%s): weapon %d -> %d\n",
                   firerId, targetId, why, chosen, override_);

        // Set AFTER the write, so a set flag always means the line is already in
        // the file rather than in progress.
        flag.store(1, std::memory_order_relaxed);
    }

    // ⚠ CALLING CONVENTION — this crashed the game once already.
    //
    // A vtable slot is invoked as __thiscall: `this` in ECX, arguments pushed on
    // the stack, callee cleans. MSVC will not let you write __thiscall on a free
    // function, so the standard model is __fastcall with a DUMMY second
    // parameter to absorb EDX:
    //
    //     int __fastcall Wrapper(This* pThis, void* /*edx*/, Arg a)
    //
    // The first version of this file declared the wrapper __stdcall(pThis,
    // pTarget). That reads `this` off the STACK — so pThis was really pTarget and
    // pTarget was whatever lay past the frame — and, worse, a 2-parameter
    // __stdcall callee pops 8 bytes while the caller pushed only 4. Four bytes of
    // stack imbalance per call, and weapon selection is called constantly, so the
    // return address was destroyed almost immediately. It surfaced as EIP landing
    // inside .rdata (0x007FA9A0) right after a paradrop spawned planes.
    //
    // Hooks.Cursor.cpp had the correct pattern the whole time; this file cited it
    // as the model and then failed to copy the signature.
    using SelectWeaponFunc         = int(__fastcall*)(TechnoClass*, void*, AbstractClass*);
    using UnitSelectWeaponFunc     = int(__fastcall*)(UnitClass*, void*, AbstractClass*);
    using InfantrySelectWeaponFunc = int(__fastcall*)(InfantryClass*, void*, AbstractClass*);

    // The firer's type extension if it has a WeaponVs rule. Only such a firer
    // gets past the wrapper's first checks, to the cache rebuild and the log.
    TechnoTypeExt::ExtData* WeaponVsExt(TechnoClass* pThis)
    {
        auto const pType = pThis->GetTechnoType();
        if (!pType)
            return nullptr;

        auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
        return pTypeExt && pTypeExt->WeaponVs.Active() ? pTypeExt : nullptr;
    }

    // The shared decision, applied to an answer the engine has already given.
    // Strictly read-only apart from the mutex-guarded log.
    int ApplyOverride(TechnoClass* pThis, AbstractClass* pTarget, int chosen)
    {
        if (!pThis || !pTarget)
            return chosen;

        auto const pTypeExt = WeaponVsExt(pThis);
        if (!pTypeExt)
            return chosen;
        auto const pType = pThis->GetTechnoType();

        // Only technos can be inhibitors or designators; a cell target cannot.
        auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
        if (!pTargetTechno)
            return chosen;

        auto const pTargetType = pTargetTechno->GetTechnoType();
        if (!pTargetType)
            return chosen;

        const auto& rule = pTypeExt->WeaponVs;

        // Inhibitor is tested first so that a type listed as BOTH resolves
        // predictably rather than by iteration order.
        const bool vsInhib = rule.VsInhibitor >= 0
                          && ActsAs(pTargetType, true, rule.InhibitorSW);
        const bool vsDesig = !vsInhib
                          && rule.VsDesignator >= 0
                          && ActsAs(pTargetType, false, rule.DesignatorSW);

        // Firer-side, checked only if no target-side rule already applied. The
        // ordering is arbitrary but FIXED, so a type configuring both never
        // depends on evaluation order.
        const bool nearInhib = !vsInhib && !vsDesig
                            && rule.WhileNearInhibitor >= 0
                            && StandingInInfluence(pThis, true);
        const bool nearDesig = !vsInhib && !vsDesig && !nearInhib
                            && rule.WhileNearDesignator >= 0
                            && StandingInInfluence(pThis, false);

        if (!vsInhib && !vsDesig && !nearInhib && !nearDesig)
            return chosen;

        const int override_ = vsInhib   ? rule.VsInhibitor
                            : vsDesig   ? rule.VsDesignator
                            : nearInhib ? rule.WhileNearInhibitor
                                        : rule.WhileNearDesignator;

        const char* why = vsInhib   ? "inhibitor"
                        : vsDesig   ? "designator"
                        : nearInhib ? "near inhibitor"
                                    : "near designator";

        LogOnce(TechnoTypeExt::UnifiedIndex(pType),
                TechnoTypeExt::UnifiedIndex(pTargetType),
                pType->ID, pTargetType->ID, why, chosen, override_);

        return override_;
    }

    // -------------------------------------------------------------------------
    // One thin wrapper per original. Each CALLS its own class's implementation
    // so every framework hook inside it still runs and decides first — then
    // ApplyOverride adjusts the answer. Pointing all three at 0x6F3330 would
    // skip UnitClass's and InfantryClass's own overrides entirely.
    // -------------------------------------------------------------------------
    // ⚠ Plain functions, deliberately not a template parameterised on the
    // original's address. DEFINE_FUNCTION_JUMP expands to `_asm mov eax,
    // <function>`, which wants a function SYMBOL; handing it a function-pointer
    // variable instead is at best ambiguous and relies on static init order for
    // a patch that is applied at load time.
    int __fastcall TechnoClass_SelectWeapon_Wrapper(TechnoClass* pThis,
                                                    void* /* unused EDX */,
                                                    AbstractClass* pTarget)
    {
        const auto original = reinterpret_cast<SelectWeaponFunc>(0x6F3330);
        return ApplyOverride(pThis, pTarget, original(pThis, nullptr, pTarget));
    }

    int __fastcall UnitClass_SelectWeapon_Wrapper(UnitClass* pThis,
                                                  void* /* unused EDX */,
                                                  AbstractClass* pTarget)
    {
        const auto original = reinterpret_cast<UnitSelectWeaponFunc>(0x746CD0);
        return ApplyOverride(pThis, pTarget, original(pThis, nullptr, pTarget));
    }

    int __fastcall InfantryClass_SelectWeapon_Wrapper(InfantryClass* pThis,
                                                      void* /* unused EDX */,
                                                      AbstractClass* pTarget)
    {
        const auto original = reinterpret_cast<InfantrySelectWeaponFunc>(0x5218E0);
        return ApplyOverride(pThis, pTarget, original(pThis, nullptr, pTarget));
    }
}

// =============================================================================
// Built on the GAME THREAD from the shared per-frame tick. Everything the
// wrapper reads is produced here, so the wrapper itself never writes and can
// safely run on PerfExt's worker threads during look-ahead target checks.
// =============================================================================
void SWExt::WeaponSelect::FrameTick()
{
    // Rebuilt when either array changes size, which is what a rules reload (new
    // scenario, different mod) looks like from here. The type count matters as
    // well as the superweapon count, because the log table is indexed by type
    // and UnifiedIndex is warmed per type.
    //
    // ⚠ This reallocates the log table, so it must not run while a worker could
    // be reading it. It cannot: PerfExt lane 3 is fork-join, entered from inside
    // one GreatestThreat call, and the game thread runs no game logic until the
    // join — so the post-loop seat this is called from can never overlap the
    // parallel phase. If lane 3 ever gains a phase that outlives the join, this
    // assumption needs revisiting.
    if (g_builtForCount != SuperWeaponTypeClass::Array.Count
        || g_builtForTypes != TechnoTypeClass::Array.Count)
    {
        BuildCache();
    }

    BuildInfluence();
}

// For PerfExt, which runs target checks (and so this wrapper) ahead of time and
// may run them on several threads: 1 if the wrapper can write for this firer,
// so its target checks must stay serial.
//
// Now always 0. Every table the wrapper reads is built on the game thread by
// FrameTick above. Three writes had to be removed to make that true:
//
//   1. the type cache, previously built lazily on first use;
//   2. the WhileNear influence list, previously built on the first weapon choice
//      of each frame (also a desync — see the file header);
//   3. TechnoTypeExt::UnifiedIndex, which caches into the type's ext on first
//      call. Easy to miss, because it reads as a pure accessor. It is now warmed
//      for every type in BuildCache, which also takes its cold path — a linear
//      TechnoTypeClass::Array.FindItemIndex — out of the per-candidate path.
//
// What remains is the diagnostic log, whose fast path is a relaxed atomic read
// and whose lock is taken only to write a line.
//
// ⚠ PerfExt's lane 3 reads this to choose parallel vs serial. Do NOT reintroduce
// a write on the wrapper's path without changing this back to 1 and telling the
// PerfExt session — a silent write here is a data race on their worker threads.
// "Pure-looking accessor with a lazy cache inside" is the shape to watch for.
extern "C" __declspec(dllexport) int __cdecl SuperWeaponExt_TargetCheckWrites(TechnoClass* /* pScanner */)
{
    return 0;
}

// Six slots: AbsVTable + 0x2E4 for each concrete TechnoClass subclass, plus the
// two abstract bases. Three different originals — see the file header. None is
// claimed by Phobos, Antares or anything in the registry.
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E2588, TechnoClass_SelectWeapon_Wrapper);    // Aircraft
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E41A0, TechnoClass_SelectWeapon_Wrapper);    // Building
DEFINE_FUNCTION_JUMP(VTABLE, 0x7E8F78, TechnoClass_SelectWeapon_Wrapper);    // FootClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F4C44, TechnoClass_SelectWeapon_Wrapper);    // TechnoClass
DEFINE_FUNCTION_JUMP(VTABLE, 0x7EB33C, InfantryClass_SelectWeapon_Wrapper);  // Infantry
DEFINE_FUNCTION_JUMP(VTABLE, 0x7F5F54, UnitClass_SelectWeapon_Wrapper);      // Unit

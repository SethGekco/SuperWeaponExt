# SuperWeaponExt — feasibility investigation

Date: 2026-08-20. Status: **investigation only, nothing built.**

Sources checked: the 12 linked Phobos PRs (via `gh api`), Antares master source
(`~/Claude/Antares-src`, commit `9f25bdb`), Phobos (`~/Claude/PrerequisiteExt/Phobos`,
`4747562` 2026-06-26, plus upstream spot-checks), the YR Hook Encyclopedia registry
(`~/Claude/YR-Hook-Encyclopedia`, 5327 rows), the Antares PDB symbol map, and a fresh
disassembly of `gamemd.exe`.

---

## 0. The headline finding

**Inhibitors, Designators, launch sites and paradrop planes are not engine code.**
They do not exist in `gamemd.exe` at all. They are C++ that lives *inside* Antares,
and — separately and redundantly — *inside* Phobos:

| | Antares | Phobos |
|---|---|---|
| `IsInhibitor` / `IsDesignator` | `src/Misc/SWTypes.cpp:231-260` | `src/Ext/SWType/SWHelpers.cpp:47-132` |
| range source | `TechnoTypeExt::{Designator,Inhibitor}Range` → `Sight` | same tags, same default |
| reached from | `NewSWType::CanFireAt`, called by Antares' own hooks | `SWTypeExt::ExtData::HasInhibitor/HasDesignator` |

Two independent implementations of the same Ares-0.A feature, with identical
semantics. The engine hooks that lead into them (`0x4AC20C` LMBUp, `0x4C78D6`
`RespondToEvent(SpecialPlace)`, `0x5098F0` AI SW fire, `0x6CEF84`
`SuperWeaponTypeClass::GetAction`) are **already taken by Antares**.

The naive read of that is "a standalone DLL can't do this" — it would have to hook
the exact addresses Antares occupies and re-decide what Antares already decided,
and Syringe chaining would make the outcome order-dependent. Phobos never attempts
it; where Phobos needs Antares' targeting verdict it calls into Antares by
**hardcoded address** (`src/Utilities/AresFunctions.h`, with separate `InitAres3_0`
/ `InitAres3_0p1` binding tables) — a version-pinned hack, not a seam to build on.

**That read is wrong, and §6 is the correction.** We do not need to replace
Antares' decision. Inhibitors and designators are *restrictions*, and restrictions
compose as AND regardless of evaluation order. A standalone DLL can carry its own
independent tag set and apply it as a pure **veto layer** at addresses Antares does
not occupy. A modder who wants our semantics simply leaves Antares'
`SW.Inhibitors`/`SW.Designators` empty, making Antares' check a no-op, and ours the
only one running. No coordination, no address contention, no version pinning.

See §6 for the three veto points, all verified unhooked.

---

## 1. Already shipped — do not build these

| Wishlist item | Where it already lives |
|---|---|
| "Superweapons initiating triggers" | **Phobos, merged.** Trigger actions `505 RunSuperWeaponAtLocation` and `506 RunSuperWeaponAtWaypoint` (`src/Ext/TAction/Body.h:17-18`). Fires any SWType at any cell/waypoint. |
| "SW assigned by hotkey" | **Phobos PR#1384, merged.** `FireTacticalSWCommandClass<0..9>` (`src/Commands/FireTacticalSW.h`), gated on `[GlobalControls] SuperWeaponSidebarKeysEnabled=yes`. *Caveat below.* |
| SW sidebar (PRs 1815 / 1711 / 1703 / 1387 / 1384) | **All merged.** Exclusive left-side SW sidebar, pyramid/rect arrangement, duplicate SW types, `TabIndex` per SW, tooltip fix. |
| PRs 1383, 1379 | **Superseded** by 1384. #1379 was closed because reviewers required `ControlClass` and scrolling; that lesson is already recorded in `~/Claude/SidebarExt/DESIGN.md`. |
| PR 1199 "Linked superweapons" | **Merged.** `SW.Link`, `.Grant`, `.Ready`, `.Reset`, roll-chances/weights. |

### Vanilla trigger actions that already touch SWs
`SetSuperweaponCharge` (charge to N%), `SuperweaponSetRechargeTime`,
`SuperweaponResetRechargeTime`, `SuperweaponReset`, `PreferredTargetCellSet` /
`…Clear` ("aim all future targetable superweapons at this waypoint"), plus the
hardcoded `NukeStrike` / `LightningStormStrike` / `IronCurtainAt` /
`MeteorShowerAt` / `LightningStrikeAt`.

`PreferredTargetCellSet` + `SetSuperweaponCharge=100` + AI auto-fire is very likely
the "CnCNet allows this partially" you remembered. Worth correcting one assumption
though: **trigger actions are not data-driven.** They are a `switch` in
`TActionClass::Execute` (`0x6DD8D7`); no INI can add one. Antares extends that switch
at `0x6DD8D7`, Phobos at `0x6DD8B0` — different addresses, no collision, and a third
DLL could take a third slot in the same prologue.

### The hotkey caveat (a real, small gap)
Phobos' 10 hotkeys are **positional** — "fire whatever is in SW-sidebar slot 3" —
and only work when the exclusive SW sidebar is enabled. There is no
`[SOMESW] HotkeyCommand=` that binds a key to a *named* SWType regardless of sidebar
state. Closing that gap is ~1 new `CommandClass` template plus an INI-driven
registration list.

---

## 2. Small upstream PRs — the genuinely easy wins

### 2a. Inhibitor / Designator relations, index, veterancy
Today (both frameworks, identically hardcoded):

```cpp
// designator: owner only
if (pTechno->Owner == pOwner) ...
// inhibitor: any non-ally only
if (!pOwner->IsAlliedWith(pTechno)) ...
```

Everything you asked for slots straight into those six small functions:

- **owner / team / ally / enemy** — the enum already exists. Antares has
  `SuperWeaponAffectedHouse` (`src/Utilities/Enums.h:68`) with
  `Owner|Allies|Enemies|Team|NotAllies|NotOwner|All`; Phobos has `AffectedHouse`.
  New tags `SW.Inhibitors.AffectsHouse=` / `SW.Designators.AffectsHouse=` are a
  one-line replacement of each hardcoded relation test.
- **index (per-SW, per-techno range)** — today range is *per TechnoType*
  (`InhibitorRange`/`DesignatorRange`, defaulting to `Sight`), so one building
  inhibits every SW at the same radius. A parallel list
  `SW.Inhibitors.Ranges=` indexed against `SW.Inhibitors=` gives per-SW radii
  without touching the TechnoType tags.
- **veterancy / elite** — `pTechno->Veterancy.IsElite()` is right there;
  `InhibitorRange.Veteran=` / `.Elite=` is a `Nullable<int>` lookup at the point
  where `pExt->InhibitorRange.Get(pType->Sight)` is read.

Estimated ~150 LOC per framework. This is the single highest-value item on the list.

### 2b. Growth / shrink over time, and proximity ratio
Same functions, but this one needs care. The range is currently computed *fresh on
every cursor frame* (`GetTargetingData` walks all of `TechnoClass::Array` each time
`GetAction` runs), so a time-varying radius is cheap to add — but it must be driven
by a **synced** clock, not render time. Use `Unsorted::CurrentFrame` or a
per-techno counter that is saved/loaded, never `GetTickCount`. See
[[kratos-rng-desync-rootcause]] for what happens when unsynced state leaks into
game logic.

The "ratio when specific buildings/units exist or are within proximity" variant is
a second `TechnoClass::Array` scan per candidate inhibitor — O(n²) on the cursor
path. Needs a per-frame cache before it ships.

### 2c. Cargo-plane direction and formation
`SW_ParaDrop::SendPDPlane` (`Antares-src/src/Misc/SWTypes/ParaDrop.cpp:292`) is 60
lines, and the entire spawn-position decision is 8 of them:

```cpp
auto edge = pOwner->StartingEdge;              // fallback: pOwner->Edge, then North
auto const spawn_cell = MapClass::Instance.PickCellOnEdge(edge, ...);  // random cell
```

Every plane in a multi-plane drop calls this independently → each picks its own
random point on the edge. That is exactly the "spawn randomly all around" you
described. Replacing it with a per-SW edge override plus a formation offset
(`SW.ParaDrop.Edge=`, `SW.ParaDrop.Formation=line|wedge|column`,
`SW.ParaDrop.Spacing=`) is a contained change to one function.

"X and Y direction based on what is nearest to spawner" is also natural here — the
launch-site building is already known to the caller, so picking the map edge nearest
that building is a few lines. **This is Antares-only** — Phobos does not implement
the ParaDrop SW at all.

### 2d. Paradrop range per superweapon
Two distinct things are hiding under this phrase; worth deciding which you mean:

- **Spread along the flight path** — already exists, but at the wrong granularity.
  Phobos' `ParadropDelay` / `ParadropEndDelay` (per AircraftType and `[General]`,
  hook `0x415A00`) sets the frames between passenger drops, which is what spaces the
  landings out. Making it *per SW* means threading the SW down to the aircraft.
- **Max distance from the launch site** — already exists as
  `SW.RangeMinimum` / `SW.RangeMaximum` (both frameworks), applied per launch site.

### 2e. Superweapon does not auto-deselect after firing
This one **is** engine code, and it is the only wishlist item that fits a standalone
DLL cleanly.

`Unsorted::CurrentSWType` @ **`0x8809A0`** is the pending-SW cursor state.
Fresh disassembly of `gamemd.exe` finds every reference:

| Address | What it does |
|---|---|
| `0x6AAE94`, `0x6AAF92` | `SidebarClass::ProcessCameoClick` — **sets** it on cameo click |
| `0x6AB2E7` | sets it to `1` (= `IronCurtain`); unverified context |
| `0x6CC46E` | inside `SuperClass::Launch`, sets it to `4` (= `ChronoWarp`) — the ChronoSphere second-click |
| `0x6CCD1C`, `0x6CCD9A`, `0x6CCE41`, `0x6CD04F`, `0x6CD2CB`, `0x6CD50F`, `0x6CD6F8`, `0x6CD7D3`, `0x6CDA53`, `0x6CDCC3`, `0x6CDE16` | **11 × `movl $-1`** — one per SW action branch inside `SuperClass::Launch` (`0x6CC390`). **This is the deselect.** |
| `0x4FB8A9`, `0x50B190` | cleared when a SW becomes unavailable (`HouseClass::UpdateSuperWeaponsOwned` region) — the correct guard to preserve |

Do **not** patch all 11 sites. Hook the entry of `SuperClass::Launch` (`0x6CC390`)
to stash the SWType, hook the exit (or `0x4C78D6` after the launch call), and if
`[SOMESW] SW.KeepSelectedAfterFire=yes` and the SW is still owned and available,
write the index back. The `0x4FB8A9`/`0x50B190` clears then still fire correctly
when the SW is genuinely lost. `0x6CC390` is **unhooked by every framework in the
registry** — clean address.

---

## 3. Real new work — not easy

### 3a. "Control units via superweapon / building placement / other technotype"
### 3b. "Units auto-move toward a technotype or the last coords dropped by a SW/weapon"

Nothing in Phobos, Antares, Kratos or the engine does this. It is a new
**standing-order** primitive on TechnoType: a persistent target-selection rule
evaluated on the unit's mission tick, with a per-house "last SW impact cell"
blackboard that superweapons write to. Phobos' `SetFollowsIndexForVehicle` (action
512) is the closest existing thing and is not close.

This is its own project, and it overlaps [[squadext-project]] (roster/leadership)
and [[traitext-project]] more than it overlaps superweapons. Recommend splitting it
out rather than bolting it onto a SW DLL.

### 3c. Not on your list but adjacent — the still-open PRs
- **#1670 Battle Points** (open, FS-21, 27 files, last touched 2026-06-30) — a whole
  second currency earned from kills, gating SW launches via `BattlePoints.Amount`.
  Live upstream work; do not duplicate.
- **#1153 SW timer restart on first click** (closed, unmerged) — `SW.FirstClickRestartsTimer`.
- **#814 spy-effect SW grant-instead-of-launch** (closed, unmerged) — small, 6 files.
- **#345 score counter + score-granted SWs** (closed, unmerged, WIP by author's own
  admission) — largely superseded in spirit by #1670.

---

## 4. Decision (Rex, 2026-08-20)

**Standalone DLL.** All four work items in scope: inhibitor/designator overhaul,
cargo-plane direction + formation, no-auto-deselect + hotkeys, unit standing orders.

The paradrop item (§2c) is the one that does *not* fit the veto model — the plane
spawn happens inside Antares' `SendPDPlane`, which no engine hook reaches, because
Antares constructs the aircraft itself. Options there, in preference order:

1. **Own the ParaDrop SW outright.** Register our own SW type and have modders use
   it instead of Antares' `Type=ParaDrop`. We then control spawn edge and formation
   completely, and never touch Antares' copy. Costs us re-implementing plane
   assembly (~120 LOC, the logic is visible in `ParaDrop.cpp`).
2. **Post-spawn correction.** Let Antares spawn the planes, then hook
   `AircraftClass::Mission_ParadropApproach` and re-place/re-vector them on their
   first tick. Cheaper, but visibly hacky and fights Antares every frame.

Recommend (1). Flagged for confirmation before implementation starts.

Unit standing orders (§3a/3b) remains genuinely independent of superweapons and
should get its own DLL even though it is in scope — its only SW contact point is
reading the per-house "last SW impact cell" blackboard that we write in the
`Fire_SW` hook anyway.

---

## 6. The veto-layer hook plan (verified against a fresh disassembly)

Three layers. Each is checked against `registry/hooks.csv` (5327 rows, all six
frameworks) and against a fresh `objdump` of `gamemd.exe`.

### Layer 1 — authoritative launch veto: `HouseClass::Fire_SW` @ **`0x4FAE50`**

This is the find that makes the whole thing work. **Every** superweapon launch in
the game funnels through this one function — 17 call sites:

| Call sites | Path |
|---|---|
| `0x4C78F3` | `RespondToEvent(SpecialPlace)` — the network-synced player-click path |
| `0x509ACD`–`0x50A480` (10 sites) | `HouseClass` AI superweapon firing, one per vanilla SW |
| `0x6EFDB4`–`0x6F030D` (5 sites) | AI script / team-mission SW actions |

And critically, the *framework* paths land here too — Antares' `SWTypeExt::TryFire`
ends in `pOwner->Fire_SW(...)` (`Hooks.Targeting.cpp:619`), which is `JMP_THIS(0x4FAE50)`,
a direct call to the game address. Phobos' SW-sidebar button and trigger actions
505/506 queue `EventType::SpecialPlace`, which arrives via `0x4C78F3`. **One hook
covers vanilla, Antares manual, Antares AI-targeting, Phobos sidebar, Phobos
triggers, and map triggers.**

- **Registry status: entry is unhooked by every framework.** Antares takes
  `0x4FAE72` — that is +0x22 *inside* the function (`HouseClass_SWFire_PreDependent`),
  and it is not on the path from the entry. No contention.
- **Stolen bytes: `0x7`.** `53` `8b d9` `8b 4c 24 08` = `push ebx; mov ebx,ecx;
  mov ecx,[esp+8]`. Lands on an instruction boundary.
- **Registers at entry:** `ECX` = `HouseClass*`, `[esp+4]` = SW index,
  `[esp+8]` = `CellStruct*`.
- **Clean abort:** set `R->AL(0)` and `return 0x4FAEF3`. At entry the stack is
  pristine (only the return address), and `0x4FAEF3` is a bare `ret $8` — exactly
  the right arg cleanup for `__thiscall bool Fire_SW(int, CellStruct const&)`.
  Do **not** jump to the real epilogue at `0x4FAEED`; it pops four registers that
  were never pushed.

Because this sits downstream of the event queue, a veto here is **network-synced by
construction** — every client evaluates it on the same frame with the same state.
That is the property that matters, and it is why this layer is the authoritative one.

### Layer 2 — click veto: `DisplayClass::LeftMouseButtonUp` @ **`0x4AC21C`**

Antares hooks `0x4AC20C` and returns to either `0x4AC21C` (SW found, `EAX` = the
`SuperWeaponTypeClass*`) or `0x4AC294` (no SW). Vanilla converges on the same two.
**`0x4AC21C` is the convergence point and is unhooked** — so a hook there runs on
the Antares path *and* the vanilla path, after either has decided.

- **Stolen bytes: `0x6`** (`8b 90 98 00 00 00` = `mov edx,[eax+0x98]`).
- **Veto:** `return 0x4AC294` — the existing "no SW here" path.

This layer is cosmetic-plus-latency only; Layer 1 is what actually enforces. Keep
them driven by one shared predicate so they can never disagree.

### Layer 3 — cursor veto: `SuperWeaponTypeClass::GetAction` @ **`0x6CEF80`**

The function's real entry is `0x6CEF80` (`0x6CEF73`–`0x6CEF7F` are alignment
`nop`s). Antares hooks `0x6CEF84`, i.e. *after* the 5-byte prologue — so
**`0x6CEF80` is free and holds exactly the 5 bytes a hook needs**
(`56` `57` `8b f9` = `push esi; push edi; mov edi,ecx`).

- **Return `0` to allow** → Syringe replays the prologue, execution reaches
  `0x6CEF84`, Antares evaluates its own constraints normally. This is what makes the
  composition AND rather than a replacement.
- **To disallow:** set `EAX` and `return 0x6CEFDB` — a bare `ret $8` past the two
  pops, correct from a stack where `esi`/`edi` were never pushed.
- **Open detail:** which disallow code to write. Antares defines its own
  `SuperWeaponAllowed = 0x7F` / `SuperWeaponDisallowed = 0x7E`
  (`src/Misc/Actions.h:9-10`); vanilla's "no" is `0x46`, returned at `0x6CEFCA`.
  An Antares-managed SW needs `0x7E`; a vanilla SW needs `0x46`. Resolve by
  checking `SWTypeExt::GetNewSWType()`-equivalence at build time — **do not guess.**

### What this buys, and what it does not

- ✅ Inhibitor/designator relations (owner/team/ally/enemy), per-SW range index,
  veterancy/elite scaling, growth-over-time, proximity ratio — all of it, under our
  own tag namespace, with no Antares contention.
- ✅ `SW.KeepSelectedAfterFire` (§2e) — `0x6CC390` is likewise unhooked.
- ✅ The per-house "last SW impact cell" blackboard for standing orders — written in
  the Layer 1 hook, which is the one place every launch is guaranteed to pass.
- ❌ We cannot *relax* an Antares restriction, only add ours. Acceptable: the modder
  leaves Antares' lists empty and uses ours.
- ❌ Paradrop plane spawn is not reachable this way (see §4).

### Cost discipline

`GetTargetingData` already walks all of `TechnoClass::Array` on **every cursor
frame**. Layer 3 runs on the same path, so a naive proximity-ratio implementation
adds a second full scan per candidate — O(n²) on the hot cursor path. Build the
per-frame cache before shipping that feature, not after.

### Desync discipline

Layer 1 is synced because it is downstream of the event queue; that property is
inherited, not automatic. Anything time-varying (growth/shrink) must be driven by
`Unsorted::CurrentFrame` or a saved/loaded per-techno counter — never `GetTickCount`,
never render-frame state, never an unsynced RNG stream. See
[[kratos-rng-desync-rootcause]] for the failure mode.

---

## 5. Encyclopedia contributions owed

Per [[hook-encyclopedia-workflow]], once anything here is exercised:

- New Tier-2 page **`Superweapon-Launch-Targeting.md`** — there is no SW page today.
  Should cover `0x6CC390` `SuperClass::Launch` + the 11 deselect sites + `0x8809A0`,
  `0x4AC20C` LMBUp, `0x4C78D6` `RespondToEvent(SpecialPlace)`, `0x6CEF84`
  `SuperWeaponTypeClass::GetAction`, `0x6AAEDF` `ProcessCameoClick_SuperWeapons`.
- **`0x4FAE50` `HouseClass::Fire_SW` deserves its own entry** — the universal
  17-call-site SW launch funnel, entry unhooked by all six frameworks, and the
  correct place to observe or veto *any* superweapon launch regardless of which
  framework initiated it. Include the clean-abort recipe (`R->AL(0)`, jump
  `0x4FAEF3`) and the trap (`0x4FAEED` corrupts the stack from an entry hook).
- **The "downstream convergence point" technique** as a general pattern: when a
  framework owns an address, hook where its `return` lands instead of fighting it —
  `0x4AC21C` (Antares returns there from `0x4AC20C`) and `0x6CEF80` (Antares hooks
  `0x6CEF84`, four bytes later, leaving the prologue free) are two worked examples.
  This generalises well beyond superweapons and is worth a README section.
- Note on **`0x6DD8D7` vs `0x6DD8B0`** — two frameworks extend the same
  `TActionClass::Execute` switch at different prologue offsets without colliding.
  Worth documenting as the canonical "how to add a trigger action" pattern.
- Note that **inhibitor/designator has no engine address at all** — a framework-level
  feature with no hook, which is exactly the kind of "does NOT exist where you'd
  expect" fact the encyclopedia's does-not sections are for.

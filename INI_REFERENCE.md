# SuperWeaponExt — INI reference

**Implemented:** inhibitors and designators (enforced at launch *and* on the
cursor, with growth/ratio range modifiers), dedicated per-superweapon hotkeys,
and an owned paradrop with spawn-edge and formation control. Everything else in
`FINDINGS.md` is not wired yet — see "Not implemented yet" below.

## Why a separate `SWExt.*` namespace

Antares already reads `SW.Inhibitors` / `SW.Designators` into its own extension,
which this DLL cannot see or modify. Rather than fight it, SuperWeaponExt parses
its own keys and applies them **on top** as an additional restriction.

Restrictions compose as AND, so:

- Set **only** `SWExt.*` and leave Antares' lists empty → our rules are the only
  ones in play. **This is the recommended setup.**
- Set both → a shot must satisfy *both* systems.
- We can never *relax* an Antares restriction, only add to it.

---

## `[SOMESW]` — SuperWeaponType

```ini
[SOMESW]
; --- inhibitors: technos that BLOCK this SW near them ---
SWExt.Inhibitors=                 ; list of TechnoTypes
SWExt.Inhibitors.Any=false        ; boolean — match ANY techno, not just the list
SWExt.Inhibitors.AffectsHouse=enemies
                                  ; owner | allies | enemies | team | notallies |
                                  ;   notowner | all | none, comma-combinable
SWExt.Inhibitors.Ranges=          ; list of ints, positionally matched to
                                  ;   SWExt.Inhibitors. -1 or absent = fall back
                                  ;   to the TechnoType's range.
SWExt.Inhibitors.RequirePower=true
                                  ; boolean — unpowered BUILDINGS stop inhibiting

; --- designators: technos REQUIRED near the target ---
SWExt.Designators=                ; list of TechnoTypes
SWExt.Designators.Any=false       ; boolean
SWExt.Designators.AffectsHouse=owner
SWExt.Designators.Ranges=         ; list of ints, as above
SWExt.Designators.RequirePower=false
```

Defaults reproduce Antares' hardcoded behaviour exactly — inhibitors count only
enemies and require powered buildings; designators count only your own and
ignore power. A mod that sets just the type lists behaves as it would under
`SW.Inhibitors` / `SW.Designators`.

### `AffectsHouse` values

Token names and bit values match Antares' `SuperWeaponAffectedHouse`, so they
read the same as `SW.AffectsHouse`.

| Token | Meaning |
|---|---|
| `owner` (`self`) | the firing house itself |
| `allies` (`ally`) | houses allied with the firer |
| `enemies` (`enemy`) | everyone else |
| `team` | `owner` + `allies` |
| `notallies` | `owner` + `enemies` |
| `notowner` | `allies` + `enemies` |
| `all` | everyone |
| `none` | nobody (disables the rule's matching) |

Comma-combinable: `SWExt.Inhibitors.AffectsHouse=owner,enemies` ≡ `notallies`.

### `Ranges` — the per-SW index

The wishlist "inhibitor/designator index". In Antares the radius lives on the
**TechnoType**, so one building inhibits every superweapon at the same distance.
`Ranges` is positional against the type list and overrides that per SW:

```ini
[NUKESPECIAL]
SWExt.Inhibitors=GAPILL,NASAM
SWExt.Inhibitors.Ranges=6,12      ; GAPILL blocks at 6, NASAM at 12

[LIGHTNINGSTORM]
SWExt.Inhibitors=GAPILL,NASAM
SWExt.Inhibitors.Ranges=20,-1     ; GAPILL blocks at 20; NASAM falls back
```

A shorter `Ranges` list is fine — the tail falls back. A longer one logs a
warning and ignores the extras.

---

## `[SOMETECHNO]` — TechnoType

```ini
[SOMETECHNO]
SWExt.InhibitorRange=             ; int, cells. Default: this type's Sight.
SWExt.InhibitorRange.Veteran=     ; int — used at veteran rank
SWExt.InhibitorRange.Elite=       ; int — used at elite rank

SWExt.DesignatorRange=
SWExt.DesignatorRange.Veteran=
SWExt.DesignatorRange.Elite=
```

Tiers inherit downward: an unset `.Elite` uses `.Veteran`, an unset `.Veteran`
uses the base, and an unset base uses `Sight` (matching Antares).

```ini
[NASAM]
SWExt.InhibitorRange=6            ; rookie
SWExt.InhibitorRange.Veteran=9
SWExt.InhibitorRange.Elite=14
```

**Interaction with `Ranges`:** a per-SW `Ranges` entry is **absolute** and skips
veterancy scaling entirely. Veterancy applies only to the TechnoType range, i.e.
when the SW gives no override. Use `-1` in `Ranges` to keep veterancy scaling
for that type.

---

## Range modifiers — growth over time, and ratio

Both are configured **per superweapon**, on either role, and both apply on top of
whatever base range resolved — including a per-SW `Ranges` override.

```ini
[SOMESW]
; --- grows (or shrinks) as the match runs on ---
SWExt.Inhibitors.Growth=          ; cells per MINUTE, FRACTIONAL ok; negative shrinks
SWExt.Inhibitors.Growth.Min=      ; int, clamp floor for the FINAL range
SWExt.Inhibitors.Growth.Max=      ; int, clamp ceiling for the FINAL range

; --- scales with how many of certain technos exist / are nearby ---
SWExt.Inhibitors.Ratio=           ; list of TechnoTypes to count
SWExt.Inhibitors.Ratio.AffectsHouse=all   ; whose technos count
SWExt.Inhibitors.Ratio.Range=0    ; cells around THE INHIBITOR to count within;
                                  ;   0 = anywhere on the map (a pure existence count)
SWExt.Inhibitors.Ratio.PerUnit=0  ; cells added per counted techno; negative shrinks
SWExt.Inhibitors.Ratio.Max=0      ; cap on the bonus's magnitude; 0 = uncapped
```

`SWExt.Designators.*` takes the identical set.

### The pipeline

```
base range   (per-SW Ranges override, else the veterancy-resolved TechnoType range)
  + Growth   (cells/minute × elapsed match time)
  + Ratio    (PerUnit × counted technos, magnitude-capped by Ratio.Max)
  clamped    to Growth.Min / Growth.Max
```

The clamps bound the **combined** result, not each term — which is what makes
"starts at 4, creeps up, never past 20" expressible in one line.

### Examples

A jammer whose reach expands through the match, capped:

```ini
[NUKESPECIAL]
SWExt.Inhibitors=NASAM
SWExt.Inhibitors.Ranges=4
SWExt.Inhibitors.Growth=2         ; +2 cells/min
SWExt.Inhibitors.Growth.Max=20    ; but never past 20
```

A shield that is only meaningful when the defender massed AA around it:

```ini
[LIGHTNINGSTORM]
SWExt.Inhibitors=NAPOWR
SWExt.Inhibitors.Ranges=0             ; useless on its own...
SWExt.Inhibitors.Ratio=NASAM,NAFLAK   ; ...until AA stands near it
SWExt.Inhibitors.Ratio.Range=8
SWExt.Inhibitors.Ratio.PerUnit=3
SWExt.Inhibitors.Ratio.Max=15
```

Decay — strong at first, fading unless something else props it up:

```ini
[PSYCHICDOMINATOR]
SWExt.Inhibitors=YAPSYT
SWExt.Inhibitors.Ranges=18
SWExt.Inhibitors.Growth=-3        ; loses 3 cells/min
SWExt.Inhibitors.Growth.Min=5     ; but never below 5
```

### Notes that matter

- **Time is the synced frame counter** (`Unsorted::CurrentFrame`), never
  wall-clock or render time, and the maths is exact integer arithmetic. Both are
  deliberate: this value gates a network-synced launch decision, so every client
  must compute the identical radius on the identical frame.
- **Growth is measured from the start of the match**, not from when the building
  was placed. Per-object age would need per-instance saved state; this does not.
- **The rate is fractional.** `Growth=0.25` is a quarter-cell per minute — one
  extra cell every four minutes. Radii are whole cells, so a slow rate simply
  holds steady between steps rather than jittering. Internally the rate is
  converted once at rules-load into thousandths of a cell per minute and every
  per-frame calculation stays exact integer, so no float reaches the synced path.
- Division **floors**, so growth and shrink of equal magnitude move the radius by
  equal amounts.
- A range that reaches `0` or below stops constraining entirely — a shrinking
  inhibitor eventually just switches off.
- **Cost**: ratio counting does *not* rescan the techno array per candidate.
  Sources and countable technos are gathered in one pass and the counted set is
  reused, so it stays O(n + k·m) rather than O(n²) on the cursor path.

---

## Semantics

- **Designators** — if none configured, always pass. Otherwise at least one
  eligible designator must be within range of the target cell.
- **Inhibitors** — if none configured, always pass. Otherwise the shot is denied
  if *any* eligible inhibitor is within range.
- A source must be alive, healthy, not in limbo, and not deactivated.
- A range of `0` or less disables that source (matches Antares' `range > 0` guard).
- Mind control does not launder an enemy into a friend: relation is computed
  from the techno's *current* controller.

---

## What is enforced, and where

Phase 1 enforces at **`HouseClass::Fire_SW`** — the launch itself. That covers
every path: manual click, AI, Antares' targeting, Phobos' SW sidebar, and
trigger actions 505/506.

The cursor is vetoed too, by two further layers that share the same predicate:
**Layer 2** (`0x4AC21C`) drops the click before it becomes a network event, and
**Layer 3** (the `GetAction` vtable slot) makes the cursor itself read
"disallowed" over a blocked cell. All three layers call one evaluator, so they
cannot disagree.

---

## Not implemented yet

Present in the design, absent from the code:

- Unit standing orders.

---

## Dedicated per-superweapon hotkey

```ini
[SOMESW]
SWExt.HotkeyIndex=          ; int 0..15, or absent for none
```

Claims one of 16 dedicated hotkey slots for **this superweapon alone**. The key
itself is bound in the in-game keyboard config, under Interface, where the entry
shows the superweapon's own `UIName`.

This is not Phobos' `FireTacticalSW1..10`. Those are *positional* — "fire
whatever sits in SW-sidebar slot N" — and require the exclusive SW sidebar.
`SWExt.HotkeyIndex` is independent of the sidebar, of whether the superweapon
has a cameo, and of `SuperWeaponSidebarKeysEnabled`.

First claimant wins if two superweapons name the same index; an out-of-range
index is ignored with a log line.

### What the key does

Controlled by `SWExt.Hotkey.FireInstantly` — see below. Either **fires
immediately** with no target, or **arms** the cursor exactly like clicking the
cameo, so the next left click fires.

### Invisible superweapon, fired at any time

`SWExt.Hotkey.FireInstantly=yes` plus `SW.ShowCameo=no` gives a superweapon with
no sidebar presence that a player triggers purely by keypress:

```ini
[MYHIDDENSW]
SW.ShowCameo=no             ; read by both Antares and Phobos
SWExt.HotkeyIndex=0
SWExt.Hotkey.FireInstantly=yes
```

Firing queues a network-synced `SpecialPlace` event rather than calling into the
engine directly, so every client executes it on the same frame. It arrives at
`HouseClass::Fire_SW`, which means the Layer 1 inhibitor/designator veto applies
to hotkey-fired superweapons automatically.

This is what the closed PR#1379 called `QuickFireAtMouse` / `QuickFireInScreen`;
here it is `SWExt.Hotkey.Target` (below).

### Optional CSF strings

| Key | Default |
|---|---|
| `TXT_SWEXT_FIRE_SW_XX` | `Fire Super Weapon %d` (fallback when the slot is unclaimed) |
| `TXT_SWEXT_FIRE_SW_XX_DESC` | `Fires the super weapon that claimed SuperWeaponExt hotkey slot %d.` |

⚠ The internal binding names (`SWExtFireSW1`..`SWExtFireSW16`) are what
`RA2MD.ini [Hotkey]` stores. They are stable and must never be renamed, or every
player's binding silently detaches.

### Fire immediately, or arm the cursor?

```ini
[SOMESW]
SWExt.Hotkey.FireInstantly=   ; boolean; absent = auto
```

| Value | Pressing the key |
|---|---|
| `yes` | **fires immediately** at no target |
| `no` | **arms** the cursor — the next left click fires |
| *absent* | auto: fires immediately only if `Action` is unset |

**⚠ Set this explicitly if Antares is loaded.** The auto path infers "no target
needed" from `Action == None`, which is correct on vanilla but never true under
Antares: Antares forces `Action = SuperWeaponAllowed` on every superweapon it
handles (`src/Ext/SWType/Body.cpp:91-93`), and via `NewSWType::FindHandler` that
is effectively all of them, vanilla types included. Leaving this unset under
Antares therefore always arms. Found by running the DLL in a real game — CI
cannot catch it.

So the invisible-superweapon recipe needs the explicit tag:

```ini
[MYHIDDENSW]
SW.ShowCameo=no
SWExt.HotkeyIndex=0
SWExt.Hotkey.FireInstantly=yes    ; required under Antares
```

Both modes are useful: arming gives a keyboard shortcut that replaces hunting
for the cameo, while instant firing gives a superweapon with no UI at all.

### Where an instant launch lands

```ini
[SOMESW]
SWExt.Hotkey.Target=mouse     ; mouse | screen | base | cell | none
SWExt.Hotkey.TargetCell=X,Y   ; only when Target=cell
```

| Value | Target cell |
|---|---|
| `mouse` (default, alias `cursor`) | the cell under the cursor when the key is pressed |
| `screen` (alias `view`) | the centre of the current view |
| `base` | the firing house's base centre (falls back to its spawn cell) |
| `cell` | the fixed `SWExt.Hotkey.TargetCell=X,Y` |
| `none` | cell `(0,0)` — for superweapons that ignore location entirely |

Only applies when the hotkey **fires**. An armed superweapon takes its cell from
the click, as always.

**AI targeting** is a separate thing and already exists: set Antares'
`SW.UseAITargeting=yes`. Antares' `SpecialPlace` handler then ignores whatever
cell we supply and runs its own target picker
(`Antares src/Ext/SWType/Hooks.Targeting.cpp:660`).

`mouse` and `screen` resolve through `DisplayClass::ProcessClickCoords`, the
engine's own screen-point-to-cell routine — the same one the real mouse handler
uses. That matters because the obvious alternative,
`TacticalClass::ClientToCoords`, assumes flat ground and lands the shot roughly
one cell north per height level on raised terrain.

**Not a desync risk**, despite reading local mouse and view state. The cell is
resolved on the pressing client and then travels *inside* the queued
`SpecialPlace` event, so every client executes the same cell — exactly how a
normal cameo click already behaves. What would be unsafe is reading the mouse
during event *execution*; we do not.

---

## Owned paradrop

`SWExt.ParaDrop=yes` makes **SuperWeaponExt** perform the drop instead of
Antares. Antares' own `SW_ParaDrop` never runs for that superweapon.

This is the one feature here that is *not* a veto layer. Antares constructs the
aircraft inside its own `SendPDPlane`, so there is no engine address to extend —
its entire spawn decision is `pOwner->StartingEdge` plus a **random**
`PickCellOnEdge`, called independently per plane. That is why stock multi-plane
drops converge from scattered points. Owning the launch is the only way to
change it.

```ini
[SOMESW]
SWExt.ParaDrop=yes
SWExt.ParaDrop.Aircraft=PDPLANE     ; AircraftType that carries the drop
SWExt.ParaDrop.Types=E1,E2          ; passengers (infantry / vehicles only)
SWExt.ParaDrop.Nums=3,2             ; counts, positional against Types

SWExt.ParaDrop.Origin=owner         ; owner | nearest | north | east | south | west
SWExt.ParaDrop.Planes=1             ; how many planes
SWExt.ParaDrop.Formation=line       ; line | column | wedge | box
SWExt.ParaDrop.Spacing=4            ; cells between drop points
SWExt.ParaDrop.Offsets=             ; "X,Y|X,Y|..." — REPLACES Formation
SWExt.ParaDrop.Delays=              ; frames to wait per plane, e.g. 0,0,45,90
```

### Spawn locale

| `Origin` | Planes enter from |
|---|---|
| `owner` *(default)* | the firing house's starting edge — what Antares does |
| `nearest` | the map edge **closest to the target**, so they come from the side you fired at |
| `north` / `east` / `south` / `west` | that edge, always |

### Formation

Patterns are laid out in **approach space** — "forward" is the flight direction,
not a map axis — so a `line` reads as planes abreast and a `column` as
nose-to-tail regardless of which edge they entered from.

| `Formation` | Shape |
|---|---|
| `line` | abreast, perpendicular to the flight path, centred on the target |
| `column` | nose-to-tail; trailing planes naturally arrive later |
| `wedge` | a V, apex leading |
| `box` | centred grid |

`Offsets` overrides the pattern entirely when you want to hand-place each plane:

```ini
SWExt.ParaDrop.Offsets=0,0|6,-3|-6,-3     ; three planes, explicit cells
```

`Delays` staggers launches in frames (15 frames = 1 second), positional per
plane. Anything past the end of the list launches immediately.

```ini
SWExt.ParaDrop.Planes=5
SWExt.ParaDrop.Delays=0,0,0,45,90         ; three now, then two more waves
```

> **Limitation:** delayed planes live in an in-memory queue that is **not saved**.
> Saving mid-drop and reloading loses any plane that had not launched. Every
> client loses the identical entries, so it cannot desync — but the planes are
> gone.

### When the planes start dropping

The engine begins releasing passengers once a plane's distance to its target
falls below `[General] ParadropRadius` — **one value governing every paradrop in
the game** (default `0x400`, i.e. 4 cells). Override it per plane type:

```ini
[SOMEAIRCRAFT]
SWExt.ParadropRadius=2048     ; leptons; 256 = 1 cell. Starts dropping 8 cells out.
```

A larger radius starts the drop earlier and spreads the units over a longer
run-in; a smaller one drops them tightly on the target.

This is per **AircraftType**, not per superweapon, because the engine's decision
point only knows the plane. Give two superweapons different plane types to give
them different drop radii.

### Notes

- The drop still obeys `SWExt.Inhibitors` / `SWExt.Designators` — the constraint
  check runs first.
- Only infantry and vehicles can be dropped; anything else is skipped with a log
  line (the same restriction Antares enforces).
- **Do not also configure Antares' `ParaDrop.*` keys** on the same superweapon.
  Ours replaces the launch entirely, so those would simply be ignored.
- Because owning the launch bypasses the engine's own bookkeeping, the recharge
  timer is reset explicitly via `SuperClass::Reset()`. If a superweapon ever
  stays permanently "Ready" after an owned drop, that is the thing to look at.

---

## Keep the superweapon selected after firing

```ini
[SOMESW]
SWExt.KeepSelectedAfterFire=no    ; boolean
```

Normally the cursor drops the superweapon the moment it fires, so re-firing
means finding the cameo again. With this set, the cursor keeps holding it —
useful for a fast- or instantly-recharging superweapon a player is expected to
use repeatedly. Right-click still puts it away.

Firing while it is still charging does nothing, exactly as clicking the cameo
early does; the cursor simply stays armed until it is ready again.

### How it works, and why it differs by path

| Superweapon | Mechanism |
|---|---|
| owned paradrop (`SWExt.ParaDrop=yes`) | we already replace the launch, so the deselect is simply skipped |
| everything else | the cursor is **re-armed on the next frame** |

The engine deselects from inside `SuperClass::Launch`, at eleven separate
`movl $-1` sites — one per superweapon action branch. Suppressing those would
mean hooking `0x6CC390`, which Antares, Ares *and* Phobos already occupy, so a
fourth handler there would be injection-order dependent. Re-arming afterwards
avoids the contention entirely.

The re-arm is **one-shot**: it fires on the frame after the launch and is then
consumed. Re-arming continuously would fight a player right-clicking to cancel.

It is also local-player-only and never serialized. `Fire_SW` runs on every
client, so an unguarded version would re-arm your cursor whenever a *remote*
player fired. The pending-superweapon index is UI state, never read by game
logic, so per-client mutation is desync-safe.

### Which way the formation faces

```ini
[SOMESW]
SWExt.ParaDrop.Formation.Align=screen   ; screen | cell | map  (or yes / no)
```

| Value | Sideways spread is laid out… |
|---|---|
| `screen` *(default, alias `yes`)* | perpendicular to the flight path **on screen** — looks abreast from every edge |
| `cell` | perpendicular in **cell** space — mathematically tidy, but looks skewed |
| `map` *(alias `no`)* | never rotates; always along cell +X |

**Why this isn't one setting.** RA2's isometric projection is not conformal — a
right angle in cell space is *not* a right angle on screen. Projecting the two
cell axes with the engine's own transform gives

```
cell +X  ->  screen (+2, +1)
cell +Y  ->  screen (-2, +1)      dot = -3, i.e. ~127° apart, not 90°
```

So a line built perpendicular to the flight path *in cell space* appears swept
back from some edges and swept forward from others — "a row in some directions,
trailing behind each other in others". `screen` corrects for the projection so
it reads the same from every approach.

Only the **sideways** axis is corrected. `column` still trails straight back
along the true flight vector, because the flight path itself already looks right.

> If planes still appear to trail when you expect a row, check `Delays=` first —
> a delayed plane launches later and will lag behind regardless of formation.

---

## Unit standing orders

A persistent per-TechnoType rule: *"whenever you have nothing better to do, head
toward X."* Set on a **TechnoType** (`[MTNK]`, `[E1]`, ...), not on a superweapon.

```ini
[MTNK]
SWExt.StandingOrder=techno            ; none (default) | techno | swimpact
SWExt.StandingOrder.Types=GAPOWR,NAPOWR   ; techno mode: what to converge on
SWExt.StandingOrder.AffectsHouse=enemies  ; owner|allies|enemies|team|notallies|notowner|all
SWExt.StandingOrder.Range=0           ; cells to search; 0 = whole map
SWExt.StandingOrder.Interval=45       ; frames between re-evaluations
SWExt.StandingOrder.IdleOnly=yes      ; only redirect units with nothing to do
```

| Mode | Destination |
|---|---|
| `none` | nothing happens (the default — costs nothing) |
| `techno` | the **nearest** live instance of any type in `.Types`, filtered by `.AffectsHouse` |
| `swimpact` | the **last cell a superweapon landed on** for that unit's own house |

`swimpact` needs no `.Types`. `techno` with no `.Types` is inert, not a wildcard —
an empty list means "converge on nothing", never "converge on everything".

### `IdleOnly`, and why it defaults to yes

A unit counts as idle when it has neither a destination nor a target. With
`IdleOnly=yes` a standing order therefore only ever fills dead time: any order
the player or the AI gives wins, and keeps winning until it completes.

`IdleOnly=no` re-issues the move every `Interval` frames regardless. That will
fight the player for control of the unit — it is meant for units that are
*supposed* to be single-minded (a berserk rush unit, a homing drone), not as a
general setting.

### `Interval` is a cost knob, not just a responsiveness knob

Units are staggered across the interval **by array index**, so with
`Interval=45` roughly 1/45th of the ordered units re-evaluate on any given
frame rather than all of them at once. Lowering it makes units react sooner and
raises the per-frame cost proportionally. `Interval=1` means every ordered unit
re-evaluates every frame; do not use it on a common unit.

### Multiplayer safety

This is the only feature in this DLL that **issues orders** rather than refusing
them, so it changes simulation state that every client must agree on. The
decision is computed only from synced state (frame counter, object array, object
coordinates), and ties between two equidistant targets resolve to the earliest
array index rather than to whichever was found first. Both properties are
covered by `tests/standingorder_test.cpp`.

### Known gap: save/load

The `swimpact` blackboard is not written into the savegame. After loading a save,
`swimpact` units have no recorded cell and are simply left alone until the next
superweapon fires. Every client loads it empty, so this cannot desync — it is a
behavioural gap, not a correctness bug.

### Drop radius, per superweapon

`SWExt.ParadropRadius=` on an **AircraftType** sets how close a plane must get to
its target before it starts dropping. That is per plane type, so two
superweapons sharing `PDPLANE` could not differ. `SWExt.ParaDrop.Radius=` sets it
on the **superweapon** instead:

```ini
[SWExtParaDropTest]
SWExt.ParaDrop=yes
SWExt.ParaDrop.Radius=2048     ; leptons; 256 = 1 cell, so 8 cells
```

Resolution order, most specific first:

1. `SWExt.ParaDrop.Radius` on the superweapon that launched the plane
2. `SWExt.ParadropRadius` on the aircraft type
3. `[General]ParadropRadius` — the engine's single global

> **Only owned drops.** A per-superweapon radius requires knowing which
> superweapon launched a given plane, which is recorded at launch — so it applies
> only where `SWExt.ParaDrop=yes`. A paradrop run by Antares never passes through
> our launch path, so it resolves at the aircraft-type level, exactly as before.

---

## Restricting a rule: by player type, and by country

Both apply to `SWExt.Inhibitors` and `SWExt.Designators` alike, using the same
`<role>.` prefix as the other sub-keys.

```ini
[SOMESW]
SWExt.Inhibitors=GAPOWR
SWExt.Inhibitors.AffectsPlayer=human       ; human | computer | both  (default both)
SWExt.Inhibitors.RequiredHouses=Americans,British
SWExt.Inhibitors.ForbiddenHouses=Russians
```

**`AffectsPlayer`** is judged on the **firing** house — "does this restriction
apply to a human, to the AI, or to both". Use it to stop an inhibitor punishing
the AI, which cannot read a cursor to understand why its shot was refused.

> A rule that does not apply reads as **inactive**, not as "active but nothing
> matched". That distinction matters for designators: an inactive designator lets
> the shot through, whereas "active with nothing in range" blocks every shot. Both
> behaviours are covered in `tests/constraint_test.cpp`.

**`RequiredHouses` / `ForbiddenHouses`** filter the **source** — the building or
unit doing the inhibiting/designating — by its owner's country. An empty
`RequiredHouses` means any country. `ForbiddenHouses` always wins, so one list
cannot resurrect what the other excluded. A source whose country cannot be
resolved satisfies only an empty `RequiredHouses`.

Names are country sections (`Americans`, `Russians`, ...). An unrecognised name
is logged and skipped rather than silently ignored, since a typo would otherwise
look exactly like the filter not working.

---

## Financial requirements

The AI fires superweapons with no regard for its bank balance. Antares'
`Money.Amount` does not help — it applies its transaction **unguarded** at
launch, so a costly superweapon still fires and can push the house negative.
These keys add the missing affordability half, and because every launch funnels
through one place, the AI is governed by them for free.

```ini
[SOMESW]
SWExt.Cost=2000                 ; credits DEDUCTED on fire; also an affordability floor
SWExt.RequiredMoney=5000        ; credits the house must HAVE and keeps (alias of .Min)
SWExt.RequiredMoney.Min=5000
SWExt.RequiredMoney.Max=8000    ; credits the house must NOT exceed
```

Each key takes an optional `.Human` or `.AI` suffix, which overrides the shared
value for that controller only:

```ini
SWExt.Cost=1000
SWExt.Cost.AI=3000              ; the AI pays a premium
SWExt.RequiredMoney.AI=10000    ; ...and waits until it is comfortable
```

| key | meaning |
|---|---|
| `Cost` | spent on fire. A house that cannot pay cannot fire. Negative = income. |
| `RequiredMoney` / `.Min` | must have at least this, and **keeps** it |
| `RequiredMoney.Max` | must have at most this |

### Notes that matter

**Overrides resolve per field, not per spec.** Setting only `SWExt.Cost.AI` does
not discard a shared `SWExt.RequiredMoney.Min` — each field falls back to the
general value independently.

**`Max=0` means "only while completely broke".** Unset is `-1`, not `0`, so a
configured zero stays meaningful. `Min` and `Max` together form a band:
`Min=2000` with `Max=8000` is a superweapon usable only in the mid-game.

**Cost and Min are different money.** `Cost` is spent; `Min` is kept. A house
holding exactly `Cost` may fire when `Min` is unset.

**Nothing is charged for a shot that does not go off.** The deduction happens
last, after the readiness check, so clicking a recharging superweapon is free.
A refused launch is never billed.

**Teaching the AI restraint** is the main use: `SWExt.RequiredMoney.AI=` alone
leaves human play untouched while stopping the AI from spending itself dry.

---

## A different weapon against designators and inhibitors

Set on the **firing** TechnoType. When its target is configured as a designator
or an inhibitor, it uses the given weapon index instead of the one the engine
picked.

```ini
[MTNK]
SWExt.Weapon.VsInhibitor=1        ; weapon index (0/1/...); unset = no override
SWExt.Weapon.VsDesignator=1
SWExt.Weapon.VsInhibitor.SW=SOMESW    ; optional: only that SW's inhibitor list
SWExt.Weapon.VsDesignator.SW=SOMESW
```

Without the `.SW` suffix the test is the **union** across every superweapon: "is
this target an inhibitor for anything". With it, the test is restricted to one
superweapon's list — the "honor indexes" case, for when a building inhibits two
superweapons and only one of them should change how it is shot at.

If a type is listed as **both**, `VsInhibitor` wins. That is arbitrary but fixed,
so it never depends on iteration order.

### How this composes with Phobos and Antares

The engine's weapon choice runs **first, in full**. Nine Phobos hooks and two
Antares hooks live inside `WhatWeaponShouldIUse` — Interceptor, MultiWeapon,
ForceFire, ForceWeapon, Gattling, Airstrike, IsLocomotor, AntiAir, NoAmmoWeapon,
Verses — and every one of them still runs and decides. This override is applied
to their answer afterwards.

That is deliberate. Replacing the function (which Antares once tried and left
commented out) would silently disable all of them.

> The override is unconditional once it applies: it does not check that the
> weapon index exists or that the weapon can actually hit the target. An index
> pointing at a weapon with no `Projectile=` will misbehave exactly as it would
> anywhere else.

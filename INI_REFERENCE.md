# SuperWeaponExt — INI reference

**Phase 1 (implemented): inhibitors and designators.** Everything else in
`FINDINGS.md` is not wired yet.

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

**Known Phase 1 gap:** the *cursor* is not yet vetoed (Layers 2 and 3 in
`FINDINGS.md` §6). Until those land, a blocked cell still shows an "allowed"
cursor and the click is silently eaten. The launch decision is correct and
network-safe; the UI just has not caught up.

---

## Not implemented yet

Present in the design, absent from the code:

- **Growth / shrink over time** — must be driven by the synced frame counter.
- **Ratio scaling vs. nearby buildings/units** — needs a per-frame cache first;
  a naive version is O(n²) on the cursor path.
- Paradrop plane direction and formation.
- `SW.KeepSelectedAfterFire`.
- Hotkey bound to a named SWType.
- Unit standing orders.

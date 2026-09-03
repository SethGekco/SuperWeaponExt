# SuperWeaponExt — hook log

Every `gamemd.exe` address this DLL touches, why it was chosen, and what was
verified vs. assumed. Per the standing rule, findings here get promoted to the
[YR Hook Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia) once
exercised in a real game.

Registry consulted: `~/Claude/YR-Hook-Encyclopedia/registry/hooks.csv`
(5327 rows; Ares, Antares, Phobos, Kratos, CnCNet-Spawner, AggressiveStance).
Disassembly: `objdump -D -b binary -m i386 --adjust-vma=0x400000 gamemd.exe`.

Status legend: **VERIFIED in-game** = exercised in a live skirmish.
**VERIFIED** = read out of a disassembly or upstream source. **ASSUMED** =
copied from a framework that uses it, not independently confirmed.

Findings from this log are now upstream in the encyclopedia as
`encyclopedia/Superweapon-Launch-Targeting.md` and the `0x692300`
`ProcessClickCoords` entry on `encyclopedia/Selection-Mouse.md` (commit
`b4d03a0`).

---

## Layer 1 — `0x4FAE50` `HouseClass::Fire_SW` — the launch veto

| | |
|---|---|
| Size | `0x7` |
| Contention | **none** — entry unhooked by all six frameworks |
| Status | **VERIFIED in-game** (2026-08-24 skirmish) + disassembly & call-site census |

The load-bearing hook. Every superweapon launch funnels through this function —
**17 call sites**, enumerated from the disassembly:

- `0x4C78F3` — `RespondToEvent(SpecialPlace)`, the network-synced player path
- `0x509ACD`, `0x509BBB`, `0x509BF8`, `0x509CAB`, `0x509DEB`, `0x509EA8`,
  `0x509F55`, `0x50A13E`, `0x50A334`, `0x50A480` — `HouseClass` AI SW firing
- `0x6EFDB4`, `0x6F0011`, `0x6F0068`, `0x6F02B6`, `0x6F030D` — AI script / team
  mission SW actions
- `0x4FAE3F` — internal tail-call

Framework paths land here too, which is what makes one hook sufficient:
Antares' `SWTypeExt::TryFire` ends in `pOwner->Fire_SW(...)`
(`Hooks.Targeting.cpp:619`), and YRpp's `Fire_SW` is `JMP_THIS(0x4FAE50)` — a
direct call to the game address, not into Antares. Phobos' SW sidebar and
trigger actions 505/506 queue `EventType::SpecialPlace` → `0x4C78F3`.

**Antares takes `0x4FAE72`** (`HouseClass_SWFire_PreDependent`) — +0x22 *inside*
the function, not on the path from the entry. No contention.

Stolen bytes (7, landing on an instruction boundary):

```
4fae50:  53              push %ebx
4fae51:  8b d9           mov  %ecx,%ebx
4fae53:  8b 4c 24 08     mov  0x8(%esp),%ecx
```

Registers at entry (nothing pushed yet): `ECX` = `HouseClass*`,
`[esp+4]` = `int idxSW`, `[esp+8]` = `CellStruct*`.

### ⚠ The abort-path trap

Jump to **`0x4FAEF3`** (a bare `ret $8`) — correct cleanup for
`__thiscall bool Fire_SW(int, CellStruct const&)` from a pristine entry stack.

Do **NOT** jump to the real epilogue at `0x4FAEED`:

```
4faeed:  5f 5e 5d        pop edi; pop esi; pop ebp
4faef0:  b0 01           mov $0x1,%al
4faef2:  5b              pop ebx
4faef3:  c2 08 00        ret $8
```

Returning a jump address means the stolen bytes are **not** executed, so at hook
time none of those four registers have been pushed. Landing on `0x4FAEED` pops
four values that were never pushed and corrupts the caller's stack.

### Desync property

This sits downstream of the event queue, so every client evaluates the veto on
the same frame against the same state. That is inherited from the position, not
from anything the hook does — and is why this, not the cursor, is authoritative.

---

## ⚠ Submodule pin — deliberate, not stale

| Submodule | Pinned commit | |
|---|---|---|
| `Phobos` | `4747562` (2026-06-26) | same as PrerequisiteExt / IntelExt |
| `YRpp` | `3ba9495` (2026-06-23) | same |

**Do not bump these casually.** Phobos `develop` has since **reworked the
extension pattern**: the wrapper class now *is* the extension
(`class SWTypeExt final : public AbstractTypeExt`, with `ExtData` deprecated as a
self-alias), and `Extension<T>::InvalidatePointer` — a **pure virtual** at
`Container.h:110` in the pinned version — no longer exists at all.

This DLL uses the pinned-era pattern (`extension_type = typename T::ExtData`,
nested `class ExtData final : public Extension<T>`), matching the other YR DLL
projects. Bumping Phobos without porting `src/Ext/*/Body.h` to the new pattern
will fail to compile — first on the now-nonexistent `InvalidatePointer` override.

---

## In-game verification (2026-08-24)

Run in a live skirmish alongside Antares + Phobos + 13 other Syringe DLLs.

| Checked | Result |
|---|---|
| Syringe handshake | recognised, handshook |
| Rules parsed | `[SWExtInhibitorTest] 3 inhibitor type(s), 0 designator type(s)` |
| Access violations | **zero**; no crash snapshots |
| Layer 1 launch veto | own Power Plant blocks the shot |
| Layers 2+3 cursor/click | cursor refuses over own Power Plant |
| **Owner-scoped inhibitors** | **own** GAPOWR blocks, **enemy** GAPOWR does not |
| Hotkey (fire + arm modes) | both behave as configured |
| Instant-fire targeting | lands on the cursor cell |
| Owned paradrop | formation flies in formation; `Origin=nearest` enters from the edge nearest the target |
| Paradrop recharge | timer resets, and the superweapon is **not** spammable while charging |
| Formation alignment | `Align=screen` reads as abreast from every approach edge |
| Growth over match time | radius widens as configured |

The owner-scoped result is the load-bearing one: Antares hardcodes inhibitors to
**enemies only** (`src/Misc/SWTypes.cpp:243-260`), so an inhibitor that triggers
on the firer's *own* building — and correctly ignores the enemy's — cannot be
Antares. It is proof the veto layers are ours and that they compose rather than
collide.

---

## Container lifecycle — ASSUMED (copied from Phobos)

Both sets are Phobos' own container sites. Ours are *separate* containers and
every handler returns 0, so Syringe chains them and the two coexist.

### `SuperWeaponTypeClass` — from Phobos `src/Ext/SWType/Body.cpp:346-392`

| Address | Size | Reg |
|---|---|---|
| `0x6CE6F6` CTOR | `0x5` | `EAX` |
| `0x6CEFE0` SDDTOR | `0x8` | `ECX` |
| `0x6CE800` + `0x6CE8D0` SaveLoad_Prefix | `0xA` / `0x8` | stack `0x4`, `0x8` |
| `0x6CE8BE` Load_Suffix | `0x7` | — |
| `0x6CE8EA` Save_Suffix | `0x3` | — |
| `0x6CEE43` LoadFromINI | `0xA` | `EBP`, stack `0x3FC` |

### `TechnoTypeClass` — from Phobos `src/Ext/TechnoType/Body.cpp:1977-2027`

| Address | Size | Reg |
|---|---|---|
| `0x711835` CTOR | `0x5` | `ESI` |
| `0x711AE0` DTOR | `0x5` | `ECX` |
| `0x7162F0` + `0x716DC0` SaveLoad_Prefix | `0x6` / `0x5` | stack `0x4`, `0x8` |
| `0x716DAC` Load_Suffix | `0xA` | — |
| `0x717094` Save_Suffix | `0x5` | — |
| `0x716123` LoadFromINI | `0x5` | `EBP`, stack `0x380` |

> **Correction worth recording.** These were first written as four per-subclass
> CTOR/DTOR pairs (Aircraft/Building/Infantry/UnitType) on the assumption that
> `TechnoTypeClass` is abstract and has no shared constructor site. That was
> wrong — Phobos hooks the shared base at `0x711835`/`0x711AE0`, and the
> invented subclass addresses were fabricated. Caught before commit by diffing
> against Phobos. **Do not write container addresses from pattern memory.**

---

## Standard DLL plumbing — ASSUMED

| Address | Size | Purpose |
|---|---|---|
| `0x7CD810` | `0x9` | `ExeRun` — apply static patches at main-loop start |
| `0x52F639` | `0x5` | `CmdLineParse` — flush the deferred debug log |

Same sites PrerequisiteExt and IntelExt use.

---

## Layer 2 — `0x4AC21C` `DisplayClass::LeftMouseButtonUp` — click veto

| | |
|---|---|
| Size | `0x6` (`8b 90 98 00 00 00` = `mov edx,[eax+0x98]`) |
| Contention | **none** — unhooked by all six frameworks |
| Status | **VERIFIED in-game** (2026-08-24 skirmish) + disassembly |

Antares hooks `0x4AC20C` and returns to either `0x4AC21C` (a superweapon
resolved; `EAX` = `SuperWeaponTypeClass*`) or `0x4AC294` (none). Vanilla's own
`call FindFirstOfAction; test eax,eax; je 0x4AC294` converges on the same two.
So `0x4AC21C` is the convergence point, and a hook there runs on the Antares
path *and* the vanilla path, after either has decided.

`ESP` is unchanged between `0x4AC21C` and `0x4AC222`, so the target cell the
game is about to put in the event is readable at `[esp+0x94]`. Veto by returning
`0x4AC294` — the existing no-superweapon path — which means the `SpecialPlace`
event is never queued at all.

---

## Layer 3 — `SuperWeaponTypeClass::GetAction` — cursor veto

| | |
|---|---|
| Mechanism | **VTABLE slot replacement** at `0x7F40FC`, *not* a code hook |
| Contention | **none** — slot unclaimed by all six frameworks |
| Status | **VERIFIED in-game** (2026-08-24 skirmish) + vtable read & disassembly |

### ⚠ Correction: `0x6CEF80` is NOT hookable

An earlier revision of this document planned a code hook at `0x6CEF80`, claiming
Antares' hook at `0x6CEF84` left "exactly the 5 bytes a hook needs". **That was
wrong — the prologue is 4 bytes:**

```
6cef80:  56           push %esi        (1)
6cef81:  57           push %edi        (1)
6cef82:  8b f9        mov  %ecx,%edi   (2)
6cef84:  83 bf ...    cmpl $0xa,...    <-- Antares' hook starts here
```

A 5-byte Syringe `jmp` at `0x6CEF80` overwrites the first byte of the
instruction at `0x6CEF84`, where Antares writes its own `jmp`. Whichever DLL
injected second would corrupt the other — a load-order-dependent crash, not a
build error. **Always count prologue bytes before claiming a hook fits.**

### What we do instead

`GetAction` is a virtual with no `call rel32` anywhere in `gamemd.exe`; its only
absolute reference is vtable slot **`0x7F40FC`** (verified to contain
`0x6CEF80`). We replace that slot via `DEFINE_FUNCTION_JUMP(VTABLE, ...)` — the
same mechanism Phobos uses at `0x7E4290`, `0x7ECD98`, `0x7E2098` — and wrap it.
The wrapper calls `0x6CEF80` directly, so Antares' in-function hook still runs
and still decides first; we only deny afterwards.

(Calling the game address directly rather than through a YRpp qualified call is
deliberate: YRpp declares this virtual `R0`, so a qualified non-virtual call
silently no-ops.)

### ✅ Resolved: which disallow code to write

Previously flagged "UNRESOLVED — do not guess." It is now read off the original's
return value rather than guessed:

| Original returned | We deny with |
|---|---|
| `0x7F` Antares `SuperWeaponAllowed` | `0x7E` Antares `SuperWeaponDisallowed` |
| anything else (vanilla) | `0x46` = `Action::NoForceShield` |

`0x46` is what vanilla itself returns at `0x6CEFCA` for a disallowed
superweapon; the enum name says `NoForceShield` because that family owns the
cursor, but the engine uses it for every SW. Denying in the caller's own
vocabulary means each system's proper no-cursor is the one that shows.

---

## Commands — `0x533066` `CommandClassCallback_Register`

| | |
|---|---|
| Size | `0x6` |
| Contention | **shared, and proven safe** |
| Status | ASSUMED (copied from Phobos), **exercised in-game** — commands registered and bound |

Registers the 16 dedicated per-superweapon hotkey slots. Antares hooks
`0x533058` (size 7); **Phobos and AggressiveStance both already hook `0x533066`**
and co-load today, so Syringe chaining at this address is demonstrated rather
than hoped for. Every handler returns 0.

---

## Planned, not yet implemented

### `0x6CC390` `SuperClass::Launch` — no-auto-deselect
Unhooked by every framework. `Unsorted::CurrentSWType` @ `0x8809A0` is cleared
by **11 `movl $-1` sites** inside `Launch`: `0x6CCD1C`, `0x6CCD9A`, `0x6CCE41`,
`0x6CD04F`, `0x6CD2CB`, `0x6CD50F`, `0x6CD6F8`, `0x6CD7D3`, `0x6CDA53`,
`0x6CDCC3`, `0x6CDE16` — one per SW action branch. Do not patch all 11; hook
entry/exit and write the index back. Preserve the clears at `0x4FB8A9` and
`0x50B190` (`HouseClass::UpdateSuperWeaponsOwned` region), which fire when a SW
is genuinely lost. Also note `0x6CC46E` sets it to `4` (= `ChronoWarp`) — the
ChronoSphere second-click, not a deselect.

---

## Encyclopedia contributions owed

1. **New Tier-2 page `Superweapon-Launch-Targeting.md`** — no SW page exists today.
2. **`0x4FAE50` entry** — the universal 17-call-site launch funnel, unhooked, plus
   the clean-abort recipe and the `0x4FAEED` trap.
3. **The "downstream convergence point" technique** — when a framework owns an
   address, hook where its `return` lands (`0x4AC21C`) or the prologue it skipped
   (`0x6CEF80`). Generalises well beyond superweapons.
4. **Inhibitors/designators have no engine address at all** — a framework-level
   feature with no hook, duplicated in Antares (`src/Misc/SWTypes.cpp:231-260`)
   and Phobos (`src/Ext/SWType/SWHelpers.cpp:47-132`). Exactly the kind of
   "does NOT exist where you'd expect" fact the does-not sections are for.

---

## ⚠ Replacing a launch at `0x4FAE50`: the three things the abort skips

Aborting `Fire_SW` to substitute your own effect is the technique this DLL's
owned paradrop uses. It works, but the abort bypasses the **entire** remainder of
the launch, and each omission showed up as a separate in-game bug. All three were
found by playing, none by CI.

| Skipped | Symptom | Fix |
|---|---|---|
| `SuperClass::ClickFire` spending the charge | superweapon stays permanently "Ready" | call `SuperClass::Reset()` (`0x6CE0B0`) |
| The deselect — 11 `movl $-1` sites for `0x8809A0`, all inside `SuperClass::Launch` | cursor keeps holding the superweapon after firing | clear `DisplayClass::Instance.CurrentSWTypeIndex` (guard on `CurrentPlayer`) |
| Every readiness check, which live *inside* `ClickFire` | superweapon is spammable while recharging — the timer resets but each click still fires | gate on `SuperClass::CanFire()` (`0x6CC360`) **before** doing the work |

The last one is the subtle one: the timer resetting *looks* like the charge is
being spent correctly, so the bug reads as "recharge is broken" when in fact
recharge is fine and the **gate** is missing. `Fire_SW`'s entry is upstream of
every check the engine makes, so anything replacing a launch there owns all of
them.

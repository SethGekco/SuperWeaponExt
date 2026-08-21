# SuperWeaponExt — hook log

Every `gamemd.exe` address this DLL touches, why it was chosen, and what was
verified vs. assumed. Per the standing rule, findings here get promoted to the
[YR Hook Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia) once
exercised in a real game.

Registry consulted: `~/Claude/YR-Hook-Encyclopedia/registry/hooks.csv`
(5327 rows; Ares, Antares, Phobos, Kratos, CnCNet-Spawner, AggressiveStance).
Disassembly: `objdump -D -b binary -m i386 --adjust-vma=0x400000 gamemd.exe`.

Status legend: **VERIFIED** = read out of a disassembly or upstream source in
this session. **ASSUMED** = copied from a framework that uses it, not
independently confirmed. **UNTESTED** = never run in a game.

---

## Layer 1 — `0x4FAE50` `HouseClass::Fire_SW` — the launch veto

| | |
|---|---|
| Size | `0x7` |
| Contention | **none** — entry unhooked by all six frameworks |
| Status | VERIFIED (disassembly + call-site census), **UNTESTED in game** |

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

## Planned, not yet implemented

### Layer 2 — `0x4AC21C` click veto
`DisplayClass::LeftMouseButtonUp`. Antares hooks `0x4AC20C` and returns to
either `0x4AC21C` (SW found, `EAX` = `SuperWeaponTypeClass*`) or `0x4AC294`
(none). Vanilla converges on the same two. **`0x4AC21C` is that convergence
point and is unhooked** — a hook there runs on the Antares path *and* the
vanilla path, after either has decided. Stolen bytes `0x6`
(`8b 90 98 00 00 00` = `mov edx,[eax+0x98]`); veto by returning `0x4AC294`.
VERIFIED by disassembly; not implemented.

### Layer 3 — `0x6CEF80` cursor veto
Real entry of `SuperWeaponTypeClass::GetAction` — `0x6CEF73`–`0x6CEF7F` are
alignment `nop`s. Antares hooks `0x6CEF84`, four bytes later, leaving the
5-byte prologue (`56 57 8b f9`) free. Return `0` to allow, so Antares still
evaluates its own constraints (this is what keeps composition AND rather than
replacement). To deny: set `EAX`, jump `0x6CEFDB` (bare `ret $8`, correct from
a stack where `esi`/`edi` were never pushed).

**UNRESOLVED — do not guess.** Which disallow code to write. Antares defines
`SuperWeaponAllowed = 0x7F` / `SuperWeaponDisallowed = 0x7E`
(`src/Misc/Actions.h:9-10`) for its own SW types; vanilla's "no" is `0x46`,
returned at `0x6CEFCA`. The right value depends on whether the SW is
Antares-managed. Resolve at runtime.

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

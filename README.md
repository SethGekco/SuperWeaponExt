# SuperWeaponExt

Standalone Syringe DLL extending Red Alert 2: Yuri's Revenge superweapons.
Built against **Antares** (not Ares) and designed to **coexist** with both
Antares and Phobos — it forks neither and contends for no address either owns.

**Status: Phase 1 scaffold. Compiles-in-CI only; never run in a game.**

---

## What it does today

Customisable **inhibitors** and **designators**, going beyond what Antares and
Phobos hardcode:

| | Antares / Phobos | SuperWeaponExt |
|---|---|---|
| Designator scope | owner only (hardcoded) | owner / allies / enemies / team / any combination |
| Inhibitor scope | enemies only (hardcoded) | same, configurable |
| Range | per **TechnoType**, defaults to `Sight` | per TechnoType **or per superweapon** (the "index") |
| Veterancy | none | separate rookie / veteran / elite radii |
| Power check | inhibitor buildings only | configurable per role |

See [INI_REFERENCE.md](INI_REFERENCE.md) for tags,
[FINDINGS.md](FINDINGS.md) for the feasibility investigation, and
[HOOKS_LOG.md](HOOKS_LOG.md) for every address touched.

---

## How it coexists

Inhibitors and designators have **no address in `gamemd.exe`**. They are C++
inside Antares (`src/Misc/SWTypes.cpp:231-260`) and, redundantly, inside Phobos
(`src/Ext/SWType/SWHelpers.cpp:47-132`). Every engine hook leading into them is
already Antares-occupied.

The way through: inhibitors and designators are *restrictions*, and restrictions
compose as **AND** regardless of evaluation order. So this DLL never replaces
Antares' decision — it carries its own `SWExt.*` tag namespace and applies it as
a **veto layer** at addresses Antares does not occupy. A mod that wants our
semantics leaves Antares' `SW.Inhibitors` empty, making its check a no-op.

The load-bearing hook is **`HouseClass::Fire_SW` @ `0x4FAE50`** — the universal
superweapon launch funnel (17 call sites: vanilla, Antares manual, Antares AI,
Phobos sidebar, Phobos trigger actions 505/506, map triggers). Its entry is
unhooked by all six frameworks in the
[YR Hook Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia).
Because it sits downstream of the event queue, the veto is network-synced by
construction.

---

## Layout

```
src/SW/Constraint.h          the decision core — ENGINE-FREE on purpose
src/Ext/SWType/Body.*        SW extension: INI parsing + the engine adapter
src/Ext/SWType/Hooks.Launch.cpp   Layer 1, the launch veto
src/Ext/TechnoType/Body.*    veterancy-tiered radii per TechnoType
tests/constraint_test.cpp    40 assertions, runs on Linux
```

`Constraint.h` includes no YRpp and touches no game type. That is deliberate:
the rules that actually decide whether a superweapon fires are the part worth
testing, and keeping them engine-free means CI can exercise them on Linux even
though the DLL itself only builds on Windows.

---

## Building

Windows/MSBuild via CI (`.github/workflows/build.yml`), matching the other
YR DLL projects.

```sh
git submodule update --init --recursive
```

Submodules are **pinned** to `Phobos@4747562` and `YRpp@3ba9495` — the same
commits PrerequisiteExt and IntelExt use. This is deliberate: Phobos `develop`
has since reworked the extension pattern (the wrapper class is now the extension,
and `Extension<T>::InvalidatePointer` was removed). Bumping without porting
`src/Ext/*/Body.h` will not compile. See `HOOKS_LOG.md`.

Constraint tests run anywhere:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/constraint_test.cpp -o ct && ./ct
```

---

## Known gaps

- **The cursor is not vetoed yet.** Layers 2 (`0x4AC21C`) and 3 (`0x6CEF80`) are
  scoped in `FINDINGS.md` §6 but unimplemented, so a blocked cell still shows an
  "allowed" cursor and the click is silently eaten. The launch decision is
  correct; the UI has not caught up.
- Range growth/shrink over time and proximity-ratio scaling are designed but not
  written — both have desync and O(n²) hazards documented at the TODO site.
- Nothing here has run in a game. Container lifecycle addresses are copied from
  Phobos and marked ASSUMED in `HOOKS_LOG.md`.

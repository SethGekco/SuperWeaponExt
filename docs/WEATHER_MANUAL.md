# Weather manual

Weather is part of **SuperWeaponExt.dll** (it used to be the separate
WeatherExt.dll). It adds three things to a mod:

- **Meters**: named numbers that rise and fall during a match (a storm level,
  radiation, heat...). Buildings, warheads and superweapons push them; they can
  climb and decline on their own; they change damage and build cost.
- **Strike pools**: turn meters into strikes. The higher the meter, the more
  strikes, and the faster they reload.
- **Strike effects**: the vanilla weather-control cloud, bolt and impact,
  rebuilt so every stage can be swapped, repeated or turned off.

Nothing happens until rulesmd.ini (or a map, or a lobby file) has a `[Weather]`
section with `Enabled=yes`.

---

## 1. How values are written

| You write | Means |
|---|---|
| `450` | a **time** written as a bare number is **frames** (15 frames = 1 second) |
| `450f` | frames, said explicitly |
| `10s` | seconds |
| `0.5m` | minutes (900 frames each) |
| `a,b` | a range: a random value from a to b each time it is used |
| `25%` | a percentage; a plain `0.25` means the same |
| `none` | turn this off (lists of anims, sounds, superweapons, effects, anchors) |
| `-1` | "no limit" where noted (`Max=-1`, `Slots.Max=-1`) |

- **Ranges written as one value.** For counts (`Burst.Count=3`) one value
  means exactly that. For distances and times (`Burst.Radius=3`,
  `Recharge.Spread=2s`) one value means 0 up to it.
- **Units in a pair.** In `Recharge.Spread=0,2s` the unit on the last value
  covers the first, so this is 0 to 2 seconds.
- **Layering.** Every key is "present-only": a later file (map, game mode,
  lobby option) only needs the keys it changes. Everything else keeps its
  value from rulesmd.ini.
- **Weight bands (pools and nothing else).** Add `.<number>` to any pool key
  and it takes over once the pool's Weight reaches that number:
  `Strike.SW.3000=HeavySW` fires HeavySW from Weight 3000 up. Bands stack: a
  band keeps every key from the bands below it unless it changes them.

## 2. Name traps

The names stayed as they are; these are the ones that catch people.

| Word | In one place | In another |
|---|---|---|
| **Amount / Weight / level** | A meter's number is its **Amount**. | A pool adds up meter Amounts into its **Weight**. The old weatherStorm.ini called the same thing a **level**. |
| **Fire.\* vs Strike.\*** | `Fire.*` on a meter is the simple way to shoot superweapons from a meter. | `Strike.*` on a pool is the full system (slots, reloads, anchors, bursts). Pick one per storm. |
| **Grant** | `Fire.GrantToOwner` (meters) | `Strike.Grant` (pools): same job. |
| **Slots** | A pool's **slots** are just its strikes. | |
| **climb / decline** | In `Motion` and `Climb.*`/`Decline.*`: which way the meter moves. | In `Recharge.Pattern`: the order strikes are sped up in. |
| **WeatherSystem.\*** | Tags on buildings, warheads and superweapons that push meters. "Weather system" = meter. | |
| **Shave** | `Recharge.Shave` = how much faster reloads get per Weight. | |
| **Edge** | On inhibitors: how far *outside* the radius. | On designators: how far *inside* it. |

---

## 3. `[Weather]`: switches

```ini
[Weather]
Enabled=yes          ; default no. Nothing runs without this.
WeatherScale=1.0     ; multiplies every WeatherSystem.Amounts push
LogInterval=150      ; frames between status lines in debug.log (0 = off)
Debug=yes            ; master switch for per-event debug lines
Debug.Lines=40       ; per category, per game (0 = unlimited)
Debug.Meters=yes     ; and Contributors, Fire, Motion, Pools, Effects, Anims,
                     ; Damage, Anchors, Bursts: each category on/off
```

## 4. `[WeatherSystems]` and `[WeatherStrikes]`

Lists of section names, like any other type list:

```ini
[WeatherSystems]     ; the meters
0=WeatherStorm

[WeatherStrikes]     ; the strike pools
0=StormStrikes
```

---

## 5. A meter

```ini
[WeatherStorm]
StartAmount=0        ; Amount at game start
Min=0
Max=100000           ; -1 = no top (held under ~1.07 billion internally)
Baseline=0           ; with Decay, the Amount drifts toward this...
Decay=0              ; ...by this much per tick (0 = no drift)...
DecayRate=15         ; ...one tick every this long
```

### Motion: the meter moving on its own

```ini
Motion=stable        ; start direction: climb | decline | stable | random
Motion.Delay=0       ; wait this long before moving
Climb.Step=10        ; add this much...
Climb.Every=1m       ; ...this often
Climb.Spread=0       ; random extra wait per step (a,b)
Climb.To=            ; where the climb ends (default Max)
Climb.AtEnd=reverse  ; reverse | hold | wrap (jump to the bottom) | stop
Climb.Pause=0        ; wait at the top before AtEnd acts
Decline.Step=10      ; the same six keys going down;
Decline.Every=1m     ; Decline.To defaults to Min
```

### Fire.\*: a meter shooting superweapons directly

The simpler, older way. For anything more than "fire X while the meter is
above Y", use a strike pool.

```ini
Fire.Types=LightningStormSpecial   ; superweapons, one slot each
Fire.Amounts=3000                  ; each starts firing at this Amount
Fire.Counts=1                      ; shots per volley
Fire.Intervals=30s                 ; time between volleys (ignores the SW's RechargeTime)
Fire.IntervalStep=-1s              ; add this to the interval...
Fire.IntervalStepAmount=1000       ; ...per this much Amount above the start
Fire.IntervalMin=10s               ; never faster than this
Fire.Target=random                 ; random | mapcenter
Fire.Owner=neutral                 ; neutral | random | <country name>
Fire.GrantToOwner=yes              ; give the owner the SW first
```

### Combat effects

Each is `1 + Mult x (Amount / PerAmount)`, capped at `MultMax`, never below 0.
`PerAmount=0` (the default) switches it off.

```ini
Firepower.PerAmount=1000   ; all damage dealt
Firepower.Mult=0.05        ; +5% per 1000 Amount
Firepower.MultMax=2.0
Armor.PerAmount=1000       ; all damage taken is divided by this one
Armor.Mult=0.05
Armor.MultMax=2.0
Cost.PerAmount=1000        ; build cost for every house
Cost.Mult=0.02
Cost.MultMax=1.5
```

### Versus: one warhead against each armor

```ini
Versus.Warheads=HE,Lightning           ; warheads this meter changes

; Per step of PerAmount:
Versus.HE.PerAmount=500
Versus.HE.Mult=0.1                     ; one value: x(1 + 0.1/step) for every armor
Versus.HE.MultMax=3.0
Versus.HE.Armors=none,light,heavy      ; names the columns of the lists below
Versus.HE.Mult=0,0.05,0.2              ; ...or one value per listed armor
Versus.HE.Add=0%,0%,10%                ; flat Verses added per step
Versus.HE.Clamp=0%,300%                ; the final Verses never leaves this range

; Keyframes: exact rows at exact Amounts, blended between them
Versus.Lightning.Keyframes=0,10000,20000
Versus.Lightning.Armors=wood,concrete
Versus.Lightning.Row.0=1.0,1.0
Versus.Lightning.Row.10000=1.5,0.8
Versus.Lightning.Row.20000=2.0,0.5
Versus.Lightning.Interp=linear         ; linear | step (hold until the next keyframe)
```

- **Mult vs Add.** `Mult` multiplies, so it can't touch an armor the warhead
  does 0% against. `Add` adds Verses directly: `Add=10%` per step makes a 0%
  armor take 10%, then 20%, and so on.
- **`Add` needs `PerAmount`.** Without `PerAmount` there are no steps, so
  `Add` does nothing.
- **Per armor or every armor.** A `Mult` or `Add` list with one value per
  `Armors=` entry is per armor; armors you leave out keep x1 and +0%. A single
  value (when `Armors=` has more than one entry, or none) covers every armor.
  A list with no `Armors=` follows the Verses order.
- **Clamp.** `Clamp` bounds the final Verses after everything else, the
  warhead's own Verses included. One value is the ceiling (floor 0%).
- **Several meters on one warhead.** Their Mults multiply, their Adds add up,
  and the tightest Clamp wins.

---

## 6. Contributors: things that push meters

On a building, unit, infantry or aircraft (pushes while it exists):

```ini
WeatherSystem.Types=WeatherStorm,Heat   ; which meters
WeatherSystem.Amounts=20,-5             ; how much, parallel to Types
WeatherSystem.Rate=1s                   ; required: push once per this long
WeatherSystem.RequiresPower=no          ; buildings: only while powered
WeatherSystem.Motion=keep               ; also steer each meter: climb | decline |
                                        ; stable | reverse | keep
```

On a warhead (pushes on every detonation):

```ini
WeatherSystem.Types=WeatherStorm
WeatherSystem.Amounts=-50
```

On a superweapon (pushes when it fires):

```ini
WeatherSystem.Types=WeatherStorm
WeatherSystem.Amounts=4000              ; on launch
WeatherSystem.Amounts.Finish=-4000      ; a second push later...
WeatherSystem.Interval=30s              ; ...this long after launch
WeatherSystem.Motion=decline
```

---

## 7. A strike pool

A pool reads meters, works out its **Weight**, keeps one strike ("slot") per
so much Weight, reloads each strike, and fires it.

### Weight and slots

```ini
[StormStrikes]
Weight.Systems=WeatherStorm    ; meters added together
Weight.Factors=1.0             ; each one's share (negative allowed)
Slots.PerWeight=10             ; one strike per 10 Weight
Slots.Min=0
Slots.Max=500                  ; -1 = the hard limit (2000)
Slots.Delay=0                  ; nothing fires before this game time
Slots.NewReady=yes             ; a new strike fires at once
Slots.Remove=newest            ; newest | oldest: which goes when Weight drops
```

### Reloads

```ini
Recharge.Max=10m               ; reload with no speed-up
Recharge.Min=                  ; fastest allowed (default = Max, i.e. no speed-up)
Recharge.Shave=0.1s,1          ; 0.1 s faster per 1 Weight
Recharge.ShaveFrom=0           ; the speed-up starts above this Weight
Recharge.Spread=0,2s           ; random extra per reload
Recharge.Pattern=global        ; how the speed-up is shared out (below)
Recharge.Pattern.Switch=never  ; never | every | random | motion | saturated
Recharge.Pattern.Every=5m      ; for every / random
Recharge.Live=no               ; yes = reloads already running adjust too
```

How the four patterns share the speed-up:
- **`global`:** every strike gets the full speed-up.
- **`even`:** the speed-up is split equally between strikes.
- **`climb`:** the first strike is fully sped up before the second gets any,
  and so on.
- **`decline`:** the same, starting from the last strike.

When to move to the next pattern in the list (`Pattern.Switch`):
- **`every`:** cycle through the list every `Pattern.Every`.
- **`random`:** jump to a random entry every `Pattern.Every`.
- **`motion`:** the first entry while the first meter climbs, the second while
  it declines, the third while it is stable.
- **`saturated`:** move on when the current pattern is fully sped up.

### What a strike does

```ini
Strike.SW=WeatherStrikeSW      ; superweapon(s) to fire; none = no superweapon
Strike.SW.Chances=             ; weights when several are listed
Strike.SW.OwnRules=yes         ; no = that SW's own scatter and SWExt.* inhibitor/
                               ; designator rules stand down for weather shots
Strike.Effect=VanillaStrike    ; effect section(s) (part 9); none = no effect
Strike.Effect.Chances=
Strike.Count=1                 ; strike points per shot
Strike.Count.Add=1,2000        ; +1 per 2000 Weight
Strike.Count.Max=0             ; 0 = the hard limit (100)
Strike.Gap=0                   ; least time between any two shots
Strike.PerFrame=2              ; at most this many shots in one frame
Strike.Target=random           ; random | mapcenter
Strike.Owner=neutral           ; neutral | random | <country> | each-human | each-player
Strike.Grant=yes               ; give the owner the SW first
```

A shot with both `Strike.SW` and `Strike.Effect` does both at the same spot.

### Anchors: inhibitors and designators

Anchors decide **where a strike may land**, based on technos on the map.
Designators pull a strike toward them; inhibitors push it off. These are the
pool's own rules, so a storm can treat a building differently from how a
player's superweapon does, and decide whose buildings count.

Each strike point is placed in this order:

1. **Pick the spot.** `Strike.Target` gives a random cell (or the map center).
2. **Designators.** The nearest designator in reach may pull the spot.
3. **Inhibitors.** If the spot is inside one, it is moved, let through, or
   cancelled.
4. **Burst.** The hits of a burst spread out from there (next section).

```ini
; --- inhibitors: keep strikes out ---
Inhibitors=GAPILL,NASAM        ; techno types | any | sw | none
                               ;   sw = the strike SW's own SWExt.Inhibitors list
Inhibitors.Ranges=6,10         ; radius per type in cells; -1 or missing = the
                               ;   type's SWExt.InhibitorRange (veterancy too), else Sight
Inhibitors.Houses=all          ; whose count, seen from the strike's owner:
                               ;   owner | allies | enemies | team | notallies |
                               ;   notowner | all | none (combine with commas)
Inhibitors.Players=all         ; all | humans | ai: owners of that kind only
Inhibitors.RequirePower=yes    ; buildings stop inhibiting when unpowered
Inhibitors.Chances=100,0,0,0   ; elsewhere, edge, ignore, cancel (weights)
Inhibitors.Edge=0,1            ; edge ring: cells outside the radius (a,b)
Inhibitors.GAPILL.Chances=0,100,0,0   ; per type, overrides Chances
```

The four inhibitor outcomes:

| Outcome | What happens |
|---|---|
| **elsewhere** | Lands at a random spot on the map outside every inhibitor. |
| **edge** | Lands on a ring just outside this inhibitor's radius, on the side facing the original spot. `Edge=0,0` hugs the radius; `Edge=2,4` lands 2-4 cells out. If another inhibitor overlaps that point, it moves further out along the same line, then tries other angles, then falls back to elsewhere. Many edge strikes together draw a visible ring. |
| **ignore** | Lands inside the inhibitor anyway. |
| **cancel** | The strike doesn't happen. |

```ini
; --- designators: pull strikes in ---
Designators=CBEBEACON          ; techno types | any | sw | none
Designators.Ranges=6
Designators.Houses=all
Designators.Players=all
Designators.RequirePower=no
Designators.Chances=0,0,0,100  ; ignore, edge, center, inside (weights)
Designators.Edge=0,1           ; edge: cells inside the radius (a,b)
Designators.Reach=0            ; pulls strikes up to this many cells beyond its
                               ;   radius; 0 = from anywhere on the map
Designators.CBEBEACON.Chances=0,20,60,20
```

The four designator outcomes (only the nearest designator in reach acts):

| Outcome | What happens |
|---|---|
| **ignore** | The strike stays where it was picked. |
| **edge** | Pulled to just inside the radius, on the side facing the original spot. A strike already inside stays put. |
| **center** | Lands on the designator itself. |
| **inside** | Lands anywhere inside the radius. |

- **Strong vs weak magnets.** Give a type high `center`/`inside` chances and
  it's a strong magnet. Give it mostly `ignore` and it's a weak one. `Reach`
  limits how far it pulls from.
- **Defaults.** Leaving out `Chances` gives inhibitors "elsewhere" and
  designators "inside", which is how Antares treats them.
- **The `sw` setting** borrows the strike superweapon's own `SWExt.Inhibitors`
  / `SWExt.Designators` types and per-SW `Ranges`. Its `AffectsHouse` and
  `RequirePower` are used too, unless the pool sets `Houses` /
  `RequirePower`. Growth and ratio ranges are not used.
- **The SW's own rules can still refuse the shot.** With
  `Strike.SW.OwnRules=yes` (the default), that superweapon's own `SWExt.*`
  rules still check the final spot and can refuse it (for example a strike
  that "ignored" an inhibitor). Set `OwnRules=no` to let the pool's anchors
  decide alone.

### Bursts: several hits around each strike point

```ini
Burst.Count=1                  ; hits per strike point, the first included (a,b)
Burst.Count.Add=1,2000         ; +1 hit per 2000 Weight
Burst.Count.Max=0              ; 0 = the hard limit (100)
Burst.Radius=0,3               ; how far each hit lands from the center, in cells (a,b)
Burst.Gap=0,10                 ; time between hits (a,b); 0 = all at once
Burst.Walk=no                  ; yes = each hit lands around the PREVIOUS hit,
                               ;   so the burst wanders like a storm cell
Burst.FirstAtCenter=yes        ; no = the first hit is spread out too
Burst.Inhibitors=yes           ; hits after the first keep out of the pool's inhibitors
Burst.SW=same                  ; same (as the shot) | none | a list of SWs
Burst.SW.Chances=
Burst.Effect=same              ; same | none | a list of effect sections
Burst.Effect.Chances=
```

A burst's hits keep landing even if the Weight changes mid-burst.

---

## 8. Strike effects: the vanilla cloud, bolt and impact

Name a section in `Strike.Effect` (or `Burst.Effect`). An empty section is the
vanilla weather-control strike: a cloud forms, the bolt fires halfway through
the cloud, then the impact, flash, damage and debris. Every key is optional
and defaults to the `[General]` value.

```ini
[VanillaStrike]
Cloud.Anims=                   ; default [General] WeatherConClouds; none = no cloud
Cloud.Height=auto              ; leptons above ground; auto = the vanilla height
Bolt.At=half                   ; when the bolt fires: half | end (of the cloud anim) | a time
Bolt.Anims=                    ; default WeatherConBolts
Bolt.Sounds=                   ; default LightningSounds
Bolt.Count=1                   ; bolts per cloud
Bolt.Every=0                   ; time between them
Bolt.Spread=0                  ; each bolt lands within this many cells
Impact.Anims=                  ; default: the warhead's own pick (WeatherConBoltExplosion)
Impact.Warhead=                ; default LightningWarhead
Impact.Damage=                 ; default LightningDamage
Impact.Flash=yes
Impact.Debris=                 ; default MetallicDebris (on bare hard ground)
Impact.Debris.Count=2,4
```

---

## 9. Recipes

**A storm that grows forever**

```ini
[WeatherStorm]
StartAmount=100
Max=-1
Motion=climb
Climb.Step=10
Climb.Every=10s
```

**Vanilla lightning, no superweapon or extra art at all**

```ini
[StormStrikes]
Weight.Systems=WeatherStorm
Strike.SW=none
Strike.Effect=VanillaStrike

[VanillaStrike]
```

**A nuke that calms the storm**

```ini
[NukeSpecial]
WeatherSystem.Types=WeatherStorm
WeatherSystem.Amounts=-2000
WeatherSystem.Motion=decline
```

**Inhibitors that ring themselves with lightning**

```ini
[StormStrikes]
Inhibitors=GAPILL,NASAM
Inhibitors.Ranges=8,12
Inhibitors.Chances=0,100,0,0   ; always to the edge
Inhibitors.Edge=0,1            ; a thin ring
Strike.SW.OwnRules=no
```

**A lightning-rod building (strong magnet) and a weak one**

```ini
[StormStrikes]
Designators=LIGHTNINGROD,RADAR
Designators.Ranges=4,6
Designators.LIGHTNINGROD.Chances=0,0,80,20   ; nearly always straight in
Designators.RADAR.Chances=80,20,0,0          ; mostly ignored
Designators.Reach=10
```

**Clustered strikes that wander**

```ini
[StormStrikes]
Burst.Count=3,6
Burst.Radius=1,3
Burst.Gap=0.5s,1.5s
Burst.Walk=yes
```

**A storm that slowly learns to hurt concrete (Verses 0%)**

```ini
[WeatherStorm]
Versus.Warheads=Lightning
Versus.Lightning.PerAmount=1000
Versus.Lightning.Armors=concrete
Versus.Lightning.Add=5%
Versus.Lightning.Clamp=0%,50%
```

---

## 10. Debugging

With `Debug=yes`, debug.log gets a line for each event in the categories you
leave on:
- **Pools:** shots and the target cell.
- **Anchors:** which inhibitor or designator acted, the outcome, and the final
  cell.
- **Bursts:** each burst and each follow-up hit.
- **Effects / Anims:** each cloud, bolt and impact, whether it was visible,
  and when it ended.
- **Damage:** every weather-changed damage value, including Add/Clamp.

At load, one line per meter and two per pool echo the settings actually read.
Check them first when something doesn't behave.

## 11. Hard limits

| What | Limit |
|---|---|
| meters | 256 |
| pools | 64 |
| slots per pool | 2,000 |
| strike points per shot / hits per burst | 100 |
| effects in flight | 1,000 |
| bursts waiting | 2,000 |
| a meter's top with `Max=-1` | ~1.07 billion |
| bolts per cloud | 100 |
| `Bolt.Spread` | 50 cells |
| `Burst.Radius` | 64 cells |

The limits keep a runaway storm from freezing the game.

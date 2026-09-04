#pragma once
/*
 * SuperWeaponExt — SuperWeaponTypeClass extension.
 *
 * Holds the parsed inhibitor/designator rules for one superweapon. The rules
 * themselves are engine-free (src/SW/Constraint.h) so they can be unit-tested
 * off-target; this class is only the INI front-end and the container plumbing.
 */
#include <SW/Constraint.h>
#include <SW/Formation.h>

#include <GeneralStructures.h>   // CellStruct — SuperWeaponTypeClass.h does not pull it directly
#include <SuperWeaponTypeClass.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

class HouseClass;
class AircraftTypeClass;
class TechnoTypeClass;

// Where the planes of an owned paradrop enter the map from.
enum class ParaDropOrigin : unsigned char
{
    Owner = 0,   // the firing house's StartingEdge — what vanilla/Antares do
    Nearest,     // the map edge closest to the TARGET cell
    North, East, South, West,
};

// A fully-specified paradrop this DLL performs itself, instead of letting
// Antares' SW_ParaDrop do it. See Hooks.ParaDrop.cpp for why owning it is the
// only way to control spawn edge and formation.
struct ParaDropConfig
{
    bool Enabled = false;

    AircraftTypeClass*            Aircraft = nullptr;
    std::vector<TechnoTypeClass*> Types;    // passengers
    std::vector<int>              Nums;     // count of each, parallel to Types

    int              Planes  = 1;
    SWExt::Formation Kind    = SWExt::Formation::Line;
    int              Spacing = 4;

    // Which space the sideways spread is laid out in. Screen is the default
    // because it is the only one that LOOKS consistent — see FormationAlign.
    SWExt::FormationAlign Align = SWExt::FormationAlign::Screen;

    // Explicit per-plane target offsets; when non-empty these REPLACE the
    // generated formation, so a modder can hand-place every plane.
    std::vector<SWExt::Offset> Offsets;

    // Frames to wait before each plane launches, parallel to plane index.
    // Anything past the end of the list launches immediately.
    std::vector<int> Delays;

    ParaDropOrigin Origin = ParaDropOrigin::Owner;
};

// Number of dedicated per-superweapon hotkey slots (src/Commands/FireNamedSW.h).
// Lives here rather than in the command header so Body.cpp can range-check
// SWExt.HotkeyIndex without a circular include. Each slot costs one entry in the
// keyboard config list.
inline constexpr int SWExtHotkeySlots = 16;

class SWTypeExt
{
public:
    using base_type = SuperWeaponTypeClass;

    // Distinct from Antares' SWType canary (0x66666666) and every other in the
    // wild — see Ext/TechnoType/Body.h for the survey.
    static constexpr DWORD Canary = 0x5B5B5B5B;

    class ExtData final : public Extension<SuperWeaponTypeClass>
    {
    public:
        SWExt::Rule Inhibitors;
        SWExt::Rule Designators;

        // Dedicated hotkey slot this superweapon claims, or -1 for none.
        //
        // TODO: SWExt.QuickFireAtMouse / .QuickFireInScreen (the closed PR#1379
        // tags) would let a TARGETED superweapon fire straight at the cursor or
        // screen centre instead of arming it. Deliberately not parsed yet — the
        // screen-point-to-cell conversion (DisplayClass::ProcessClickCoords plus
        // wherever the live mouse position lives) has not been verified, and a
        // silently-inert INI key is worse than an absent one.
        int HotkeyIndex;

        ParaDropConfig ParaDrop;

        // Leave the cursor holding this superweapon after it fires, instead of
        // deselecting. Lets a player re-fire a fast-recharging superweapon
        // without hunting for the cameo again.
        bool KeepSelectedAfterFire;

        // Does the hotkey FIRE the superweapon outright, or just arm the cursor?
        //   -1 = auto (infer from Action == None), 0 = always arm, 1 = always fire
        //
        // ⚠ WHY THIS IS NOT INFERRED. The obvious rule — "Action == Action::None
        // means no target is needed" — is unusable whenever Antares is loaded.
        // Antares FORCES Action = SuperWeaponAllowed (0x7F) on every superweapon
        // it handles (Antares src/Ext/SWType/Body.cpp:91-93), and via
        // NewSWType::FindHandler that is essentially all of them, vanilla types
        // included. So the auto path never triggers under Antares. Found by
        // running the DLL in a real game, not by CI: the hotkey armed the SW
        // instead of firing it. Modders on Antares must set this explicitly.
        int HotkeyFireInstantly;

        // Where an instantly-fired hotkey launch aims.
        //
        // Only matters when the hotkey FIRES rather than arms — an armed
        // superweapon gets its cell from the click, as usual.
        //
        // NOT a desync risk despite reading local mouse/view state: the cell is
        // resolved on the pressing client and then travels inside the queued
        // SpecialPlace event, so every client executes the same cell. Exactly
        // how a normal cameo click already works.
        enum class HotkeyTargetMode : unsigned char
        {
            Mouse = 0,   // cell under the cursor (default)
            Screen,      // centre of the current view
            Base,        // the firing house's base centre
            Cell,        // fixed HotkeyTargetCell
            None,        // cell (0,0) — for superweapons that ignore location
        };

        HotkeyTargetMode HotkeyTarget;
        CellStruct HotkeyTargetCell;

        // Resolve HotkeyTarget to an actual cell for pFirer.
        CellStruct ResolveHotkeyCell(HouseClass* pFirer) const;

        explicit ExtData(SuperWeaponTypeClass* pOwner)
            : Extension<SuperWeaponTypeClass>(pOwner)
            , Inhibitors{}
            , Designators{}
            , HotkeyIndex(-1)
            , KeepSelectedAfterFire(false)
            , HotkeyFireInstantly(-1)
            , HotkeyTarget(HotkeyTargetMode::Mouse)
            , HotkeyTargetCell{}
        { }

        virtual ~ExtData() = default;

        virtual void LoadFromINIFile(CCINIClass* pINI) override;

        // Only ints are stored — nothing to invalidate.
        virtual void InvalidatePointer(void*, bool) override { }

        virtual void LoadFromStream(PhobosStreamReader& stm) override;
        virtual void SaveToStream(PhobosStreamWriter& stm) override;

        // True when the modder configured neither role, i.e. this superweapon is
        // not ours to police and every hook must bail out immediately.
        bool IsConfigured() const
        {
            return this->Inhibitors.Active() || this->Designators.Active();
        }

        // THE decision. Walks TechnoClass::Array, reduces each techno to a
        // SWExt::Source, and asks the pure core. Returns true when the shot is
        // permitted.
        bool AllowsFireAt(HouseClass* pFirer, const CellStruct& cell) const;

        // Explain a refusal: names the inhibitor that blocked, its resolved
        // radius, and how that radius was arrived at (base / growth / ratio).
        //
        // ⚠ Call from a per-CLICK site, not the per-frame cursor evaluation.
        // Originally wired to the launch path, where it never fired once: the
        // Layer 2 click veto stops the SpecialPlace event being queued, so
        // Fire_SW is never reached for a player-initiated refusal. Zero denial
        // lines were logged across every test session because of that.
        void LogDenial(HouseClass* pFirer, const CellStruct& cell) const;

        // As LogDenial, but prints only when the explanation changes. `sig` is
        // in/out: the caller keeps it between calls. Used by the cursor layer,
        // which is evaluated on every mouse move.
        void LogDenialSig(HouseClass* pFirer, const CellStruct& cell,
                          int& sig) const;

        // Reduce the live techno array to the evaluator's plain-data view.
        void GatherSources(HouseClass* pFirer,
                           std::vector<SWExt::Source>& sources,
                           SWExt::EvalContext& ctx) const;

    private:
        template <typename T> void Serialize(T& stm);
    };

    class ExtContainer final : public Container<SWTypeExt>
    {
    public:
        ExtContainer();
        ~ExtContainer();
    };

    static ExtContainer ExtMap;

    // Shared predicate for the two CURSOR-side layers (click veto @0x4AC21C and
    // the GetAction vtable wrapper). Resolves the ext, early-outs when this SW
    // is not ours to police, and evaluates against HouseClass::CurrentPlayer —
    // the cursor is always the local player's.
    //
    // Layer 1 calls ExtData::AllowsFireAt directly with the *firing* house,
    // because the launch path also runs for AI and remote players. Both routes
    // reach the same evaluator, which is what stops the layers disagreeing.
    static bool AllowsCursorAt(SuperWeaponTypeClass* pType, const CellStruct& cell);

    // Explain a CLICK that was refused by the cursor layer. Fires once per
    // click, which is why the diagnostic lives here and not on the launch path:
    // Layer 2 stops the click before an event is ever queued, so Fire_SW is
    // never reached and a launch-side log would stay silent forever.
    static void LogCursorDenial(SuperWeaponTypeClass* pType, const CellStruct& cell);

    // Layer 3's refusal log. Separate from LogCursorDenial because Layer 3 runs
    // on EVERY mouse move, so it is throttled; Layer 2 fires once per click and
    // is not. See the comment on the definition for why this layer needed its
    // own logger at all.
    static void LogCursorRefusal(SuperWeaponTypeClass* pType, const CellStruct& cell);

    // --- dedicated per-superweapon hotkeys (src/Commands/FireNamedSW.h) ---

    // The superweapon that claimed hotkey slot `index`, or nullptr. First
    // claimant wins; duplicates are reported at parse time.
    static SuperWeaponTypeClass* FindByHotkeyIndex(int index);

    // Fire the superweapon bound to `index` for the local player. Queues a
    // network-synced SpecialPlace event, exactly as the sidebar cameo does — so
    // it lands in HouseClass::Fire_SW and passes through the Layer 1 veto.
    static void FireByHotkeyIndex(int index);

    // --- owned paradrop (Hooks.ParaDrop.cpp) ---

    // Perform this superweapon's paradrop at `cell`. Returns true when it
    // handled the launch, in which case the caller must ABORT the normal path
    // so Antares' own SW_ParaDrop never runs for the same shot.
    static bool RunOwnedParaDrop(SuperWeaponTypeClass* pType, HouseClass* pFirer,
                                 const CellStruct& cell);

    // Ticked once per frame; launches any planes whose delay has elapsed and
    // re-arms the cursor for a KeepSelectedAfterFire superweapon.
    static void TickPendingParaDrops();

    // Ask for the cursor to be put back on `swIndex` next frame, after the
    // engine has finished deselecting it. Local-player only.
    static void RequestKeepSelected(int swIndex);
};

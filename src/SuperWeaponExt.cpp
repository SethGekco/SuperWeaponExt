#include "SuperWeaponExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

HANDLE SuperWeaponExtDLL::hInstance = nullptr;

char SuperWeaponExtDLL::readBuffer[SuperWeaponExtDLL::readLength];
wchar_t SuperWeaponExtDLL::wideBuffer[SuperWeaponExtDLL::readLength];

void SuperWeaponExtDLL::ExeRun()
{
    Patch::ApplyStatic();

    // Unlike PrerequisiteExt, we do NOT require an Ares-lineage DLL: our veto
    // layer hooks HouseClass::Fire_SW (0x4FAE50), a vanilla engine funnel that
    // exists with or without Antares. Log what we found anyway — if Antares is
    // present, its own SW.Inhibitors/SW.Designators still apply *in addition*
    // to ours (restrictions compose as AND), and a modder seeing "denied" for
    // an unexpected reason will want to know which system said no.
    if (GetModuleHandleA("Antares.dll"))
    {
        Debug::Log("[SuperWeaponExt] Antares detected. Its SW.Inhibitors/"
                   "SW.Designators still apply alongside SWExt.* — leave the "
                   "Antares lists empty if you want ours to be the only rules.\n");
    }
    else if (GetModuleHandleA("Ares.dll"))
    {
        // Antares reports itself as "Ares 3.0p1", so the module name alone is
        // not proof of real Ares. Either way the note above applies.
        Debug::Log("[SuperWeaponExt] An Ares-lineage DLL is loaded (note: Antares "
                   "presents the Ares identity). Its SW targeting rules apply "
                   "alongside SWExt.*.\n");
    }
    else
    {
        Debug::Log("[SuperWeaponExt] No Ares-lineage DLL detected; SWExt.* rules "
                   "are the only superweapon targeting constraints in play.\n");
    }
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        SuperWeaponExtDLL::hInstance = hInstance;
        Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
    }
    return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
    pInfo->Message = const_cast<char*>("SuperWeaponExt");
    return S_OK;
}

// Game main-loop start — apply static patches at the right time.
DEFINE_HOOK(0x7CD810, ExeRun, 0x9)
{
    SuperWeaponExtDLL::ExeRun();
    return 0;
}

// Flush deferred debug log after command-line parse.
DEFINE_HOOK(0x52F639, CmdLineParse, 0x5)
{
    Debug::LogDeferredFinalize();
    return 0;
}

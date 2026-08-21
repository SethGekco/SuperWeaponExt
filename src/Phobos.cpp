#include <Phobos.h>

// Minimal Phobos static-member definitions required by Container.h and other
// Phobos utilities we link against. We define only what this project needs.

HANDLE Phobos::hInstance = nullptr;

char Phobos::readBuffer[Phobos::readLength];
wchar_t Phobos::wideBuffer[Phobos::readLength];

// Stubs for Phobos lifecycle methods we don't use.
void Phobos::CmdLineParse(char**, int) { }
void Phobos::ExeRun() { }
void Phobos::ExeTerminate() { }

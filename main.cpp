// ---------------------------------------------------------
// MAIN ENTRY POINT
// ---------------------------------------------------------
#include <windows.h>
#include "d3d9_hook.h"

// DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // Bring the hook up off the loader lock: the D3D9 device is created
        // later by the game, and the hook has to wait for it anyway.
        if (HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)D3D9Hook::Initialize,
                                          hModule, 0, nullptr))
            CloseHandle(hThread);
        break;

    case DLL_PROCESS_DETACH:
        D3D9Hook::Shutdown();
        break;
    }
    return TRUE;
}

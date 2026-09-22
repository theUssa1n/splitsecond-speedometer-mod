// ---------------------------------------------------------
// D3D9 HOOK HEADER
// ---------------------------------------------------------
#pragma once
#include <d3d9.h>

// Link to DirectX libs
#pragma comment(lib, "d3d9.lib")

namespace D3D9Hook {
    // ---------------------------------------------------------
    // MAIN FUNCTIONS
    // ---------------------------------------------------------
    void Initialize(HMODULE hModule);
    void Shutdown();

    // ---------------------------------------------------------
    // FUNCTION TYPEDEFS
    // ---------------------------------------------------------
    // The function prototype for EndScene
    typedef HRESULT(APIENTRY* tEndScene)(LPDIRECT3DDEVICE9 pDevice);

    // ---------------------------------------------------------
    // GLOBAL VARIABLES
    // ---------------------------------------------------------
    // Global variable to hold the original function
    extern tEndScene oEndScene;
}

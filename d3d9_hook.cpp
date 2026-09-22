// ---------------------------------------------------------
// D3D9 HOOK
//
// The mod owns a single hook: EndScene. Once per frame it reads the game state,
// polls the unit hotkey and hands the current speed of every local player to the
// game's own HUD element. The mod draws nothing itself.
// ---------------------------------------------------------
#include "d3d9_hook.h"

#include <windows.h>
#include <cstdint>

#include "hud_speed.h"
#include "minhook/MinHook.h"

// ---------------------------------------------------------
// GLOBAL VARIABLES
// ---------------------------------------------------------
D3D9Hook::tEndScene D3D9Hook::oEndScene = nullptr;
void* d3d9Device[119];  // Storage for the VTable
bool attached = false;

// ---------------------------------------------------------
// GAME STATE
// ---------------------------------------------------------
// The in-game UI object only exists while a race HUD is up, and its view state
// at +8 says which screen is showing:
//   0 = racing       1 = modal
//   2, 3 = paused    4 = race results     5 = loading
// The last four do not show the race HUD. The engine's own "is the game paused"
// helper is nothing more than (state == 2 || state == 3), so reading the field
// here is cheaper - and build independent - than calling into the exe.
bool InRace() {
    const uintptr_t uiPtr = GameBuild::InGameUiPtr();
    if (!uiPtr) return false;
    if (IsBadReadPtr(reinterpret_cast<void*>(uiPtr), sizeof(uint32_t))) return false;

    const uintptr_t inGameUi = *reinterpret_cast<uint32_t*>(uiPtr);
    if (!inGameUi) return false;
    if (IsBadReadPtr(reinterpret_cast<void*>(inGameUi + 8), sizeof(int))) return false;

    const int viewState = *reinterpret_cast<int*>(inGameUi + 8);
    return !(viewState == 2 || viewState == 3 || viewState == 4 || viewState == 5);
}

// ---------------------------------------------------------
// HOOKS: ENDSCENE
// ---------------------------------------------------------
HRESULT APIENTRY hkEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!attached) return D3D9Hook::oEndScene(pDevice);

    const bool inRace = InRace();

    // 'M' switches miles / kilometres. It is polled here rather than hooked into
    // the game's window procedure, so the mod never touches the message pump and
    // never swallows input the game wants.
    static bool mWasDown = false;
    const bool mIsDown = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mIsDown && !mWasDown && inRace) HudSpeed::ToggleUnit();
    mWasDown = mIsDown;

    HudSpeed::Update(inRace);

    return D3D9Hook::oEndScene(pDevice);
}

// ---------------------------------------------------------
// HELPER FUNCTIONS
// ---------------------------------------------------------
// Function to find the D3D9 Device VTable address
bool GetD3D9Device(void** pTable, size_t Size) {
    if (!pTable) return false;

    // Create a dummy D3D9 object
    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) return false;

    // Create our own hidden window: GetForegroundWindow() can return NULL at
    // injection time (no foreground window yet), which makes CreateDevice fail
    // and silently kills the whole mod.
    HWND hDummyWnd = CreateWindowExA(0, "STATIC", "D3D9Hook", WS_POPUP,
                                     0, 0, 100, 100, NULL, NULL,
                                     GetModuleHandleA(nullptr), nullptr);
    if (!hDummyWnd) {
        pD3D->Release();
        return false;
    }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = hDummyWnd;

    IDirect3DDevice9* pDummyDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hDummyWnd,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);

    bool ok = false;
    if (SUCCEEDED(hr) && pDummyDevice) {
        // Copy the VTable
        memcpy(pTable, *reinterpret_cast<void***>(pDummyDevice), Size);
        pDummyDevice->Release();
        ok = true;
    }

    DestroyWindow(hDummyWnd);
    pD3D->Release();
    return ok;
}

// ---------------------------------------------------------
// INITIALIZATION
// ---------------------------------------------------------
void D3D9Hook::Initialize(HMODULE /*hModule*/) {
    // Every address this mod uses is hardcoded for one build. On anything else
    // (an unknown exe, a future patch) they belong to unrelated memory, so the
    // mod stays completely idle: no hook, no scan, no writes.
    if (GameBuild::Detect() == GameBuild::Id::Unknown) return;

    // 1. Get D3D9 VTable
    if (!GetD3D9Device(d3d9Device, sizeof(d3d9Device))) return;

    // 2. Initialize MinHook
    if (MH_Initialize() != MH_OK) return;

    // 3. Create the hook for EndScene (VTable index 42)
    if (MH_CreateHook(d3d9Device[42], reinterpret_cast<void*>(&hkEndScene),
                      reinterpret_cast<void**>(&oEndScene)) != MH_OK)
        return;

    // 4. Enable hooks
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return;

    attached = true;
    HudSpeed::Init();
}

// ---------------------------------------------------------
// SHUTDOWN
// ---------------------------------------------------------
void D3D9Hook::Shutdown() {
    attached = false;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

//==============================================================================
// SpecularLightingMod.asi - Enable Specular Lighting in GTA:SA
// Target: GTA SA v1.0 US
// Compile: cl /LD /O2 /arch:SSE2 /MT SpecularLightingMod.cpp /link /OUT:SpecularLightingMod.asi d3d9.lib
// Install: Copy SpecularLightingMod.asi to GTA SA folder (requires ASI Loader)
//==============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

#pragma comment(lib, "d3d9.lib")

//==============================================================================
// CONFIGURATION
//==============================================================================

#define ENABLE_DEBUG_OUTPUT 1  // Set to 0 to disable DebugView messages
#define SPECULAR_POWER 80.0f   // How "shiny" surfaces are (20-200, higher = smaller highlights)
#define SPECULAR_INTENSITY 1.5f // How bright specular highlights are (0.5-3.0)

//==============================================================================
// GAME ADDRESSES (GTA SA 1.0 US)
//==============================================================================

namespace Addrs {
    // D3D Device pointer
    IDirect3DDevice9** ppDevice = (IDirect3DDevice9**)0xC97C28;

    // RenderWare functions that set render states
    void* RwD3D9SetRenderState = (void*)0x7FC2D0;
}

//==============================================================================
// GLOBALS
//==============================================================================

static IDirect3DDevice9* g_pDevice = nullptr;
static bool g_bInitialized = false;
static bool g_bSpecularEnabled = true;
static DWORD g_FrameCount = 0;

// Material properties
static float g_fSpecularPower = SPECULAR_POWER;
static float g_fSpecularIntensity = SPECULAR_INTENSITY;

//==============================================================================
// UTILITIES
//==============================================================================

void SafePatch(void* addr, const void* data, size_t size) {
    DWORD oldProtect;
    VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(addr, data, size);
    VirtualProtect(addr, size, oldProtect, &oldProtect);
}

void DebugLog(const char* msg) {
#if ENABLE_DEBUG_OUTPUT
    OutputDebugStringA(msg);
#endif
}

//==============================================================================
// VTABLE HOOKING
//==============================================================================

bool HookVTableMethod(void* pInterface, DWORD dwIndex, void* pHook, void** ppOriginal) {
    if (!pInterface) return false;

    void** pVTable = *(void***)pInterface;
    if (!pVTable) return false;

    DWORD dwOld;
    if (!VirtualProtect(&pVTable[dwIndex], sizeof(void*), PAGE_READWRITE, &dwOld)) {
        return false;
    }

    if (ppOriginal) {
        *ppOriginal = pVTable[dwIndex];
    }

    pVTable[dwIndex] = pHook;
    VirtualProtect(&pVTable[dwIndex], sizeof(void*), dwOld, &dwOld);

    return true;
}

//==============================================================================
// D3D9 HOOKS
//==============================================================================

typedef HRESULT(WINAPI* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI* SetMaterial_t)(IDirect3DDevice9*, CONST D3DMATERIAL9*);
typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);

SetRenderState_t g_OrigSetRenderState = nullptr;
SetMaterial_t g_OrigSetMaterial = nullptr;
Present_t g_OrigPresent = nullptr;

// Hook SetRenderState to force specular enable
HRESULT WINAPI Hook_SetRenderState(IDirect3DDevice9* device,
    D3DRENDERSTATETYPE state,
    DWORD value) {
    if (!g_bInitialized || !g_bSpecularEnabled) {
        return g_OrigSetRenderState(device, state, value);
    }

    // Intercept attempts to disable specular and keep it enabled
    if (state == D3DRS_SPECULARENABLE) {
        if (value == FALSE) {
            value = TRUE; // Force enable!
        }
    }

    return g_OrigSetRenderState(device, state, value);
}

// Hook SetMaterial to add specular properties
HRESULT WINAPI Hook_SetMaterial(IDirect3DDevice9* device, CONST D3DMATERIAL9* pMaterial) {
    if (!g_bInitialized || !g_bSpecularEnabled || !pMaterial) {
        return g_OrigSetMaterial(device, pMaterial);
    }

    // Create modified material with specular
    D3DMATERIAL9 modifiedMat = *pMaterial;

    // Add specular component based on diffuse color
    // This makes materials that are bright have more specular
    float avgColor = (modifiedMat.Diffuse.r + modifiedMat.Diffuse.g + modifiedMat.Diffuse.b) / 3.0f;

    modifiedMat.Specular.r = avgColor * g_fSpecularIntensity;
    modifiedMat.Specular.g = avgColor * g_fSpecularIntensity;
    modifiedMat.Specular.b = avgColor * g_fSpecularIntensity;
    modifiedMat.Specular.a = 1.0f;
    modifiedMat.Power = g_fSpecularPower;

    return g_OrigSetMaterial(device, &modifiedMat);
}

// Hook Present for per-frame updates
HRESULT WINAPI Hook_Present(IDirect3DDevice9* device, CONST RECT* pSourceRect,
    CONST RECT* pDestRect, HWND hDestWindowOverride,
    CONST RGNDATA* pDirtyRegion) {
    g_FrameCount++;

    // Print status every 5 seconds
    if (g_FrameCount % 300 == 0 && g_bSpecularEnabled) {
        char buf[128];
        sprintf(buf, "[Specular] Active - Power: %.1f, Intensity: %.1f\n",
            g_fSpecularPower, g_fSpecularIntensity);
        DebugLog(buf);
    }

    return g_OrigPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

//==============================================================================
// SPECULAR LIGHT SETUP
//==============================================================================

void SetupSpecularLight() {
    if (!g_pDevice) return;

    // Create a directional light for specular highlights
    D3DLIGHT9 specLight;
    memset(&specLight, 0, sizeof(D3DLIGHT9));

    specLight.Type = D3DLIGHT_DIRECTIONAL;

    // White light
    specLight.Diffuse.r = 1.0f;
    specLight.Diffuse.g = 1.0f;
    specLight.Diffuse.b = 1.0f;
    specLight.Diffuse.a = 1.0f;

    // Bright specular component
    specLight.Specular.r = 1.0f;
    specLight.Specular.g = 1.0f;
    specLight.Specular.b = 1.0f;
    specLight.Specular.a = 1.0f;

    // Direction (from upper-left, like sun)
    specLight.Direction.x = -0.3f;
    specLight.Direction.y = -0.8f;
    specLight.Direction.z = -0.5f;

    // Set light at index 2 (avoid conflicts with game's lights 0 and 1)
    g_pDevice->SetLight(2, &specLight);
    g_pDevice->LightEnable(2, TRUE);

    DebugLog("[OK] Specular light configured\n");
}

//==============================================================================
// INITIALIZATION
//==============================================================================

void InitializeSpecular() {
    if (g_bInitialized) return;

    char buf[256]; // Declare buffer here for the entire function

    DebugLog("\n========================================\n");
    DebugLog("  Specular Lighting Mod v1.0\n");
    DebugLog("========================================\n");
    DebugLog("[Init] Initializing...\n");

    // Wait for D3D9 device
    int timeout = 200;
    while ((!Addrs::ppDevice || !*Addrs::ppDevice) && timeout-- > 0) {
        Sleep(100);
    }

    if (!Addrs::ppDevice || !*Addrs::ppDevice) {
        DebugLog("[ERROR] Could not find D3D9 device\n");
        return;
    }

    g_pDevice = *Addrs::ppDevice;

    // Validate device
    __try {
        UINT refs = g_pDevice->AddRef();
        g_pDevice->Release();

        if (refs == 0) {
            DebugLog("[ERROR] Device is invalid\n");
            g_pDevice = nullptr;
            return;
        }

        sprintf(buf, "[OK] Device: 0x%p (refs: %u)\n", g_pDevice, refs);
        DebugLog(buf);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DebugLog("[ERROR] Device access violation\n");
        g_pDevice = nullptr;
        return;
    }

    // Hook D3D9 functions
    bool success = true;

    if (HookVTableMethod(g_pDevice, 57, Hook_SetRenderState, (void**)&g_OrigSetRenderState)) {
        DebugLog("[OK] SetRenderState hooked\n");
    }
    else {
        DebugLog("[WARNING] SetRenderState hook failed\n");
        success = false;
    }

    if (HookVTableMethod(g_pDevice, 49, Hook_SetMaterial, (void**)&g_OrigSetMaterial)) {
        DebugLog("[OK] SetMaterial hooked\n");
    }
    else {
        DebugLog("[WARNING] SetMaterial hook failed\n");
        success = false;
    }

    if (HookVTableMethod(g_pDevice, 17, Hook_Present, (void**)&g_OrigPresent)) {
        DebugLog("[OK] Present hooked\n");
    }
    else {
        DebugLog("[WARNING] Present hook failed\n");
    }

    if (!success) {
        DebugLog("[ERROR] Critical hooks failed\n");
        return;
    }

    // Enable specular rendering
    g_pDevice->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    g_pDevice->SetRenderState(D3DRS_LOCALVIEWER, FALSE); // Cheaper specular calculation

    DebugLog("[OK] Specular render states set\n");

    // Set up specular light
    SetupSpecularLight();

    g_bInitialized = true;
    g_bSpecularEnabled = true;

    DebugLog("\n========================================\n");
    DebugLog("  SPECULAR LIGHTING ACTIVE!\n");
    sprintf(buf, "  Power: %.1f | Intensity: %.1f\n", g_fSpecularPower, g_fSpecularIntensity);
    DebugLog(buf);
    DebugLog("========================================\n\n");
}

//==============================================================================
// INIT THREAD
//==============================================================================

DWORD WINAPI InitThread(LPVOID) {
    DebugLog("[Init] Waiting 15 seconds for game...\n");
    Sleep(15000);

    __try {
        InitializeSpecular();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DebugLog("[CRITICAL] Initialization crashed!\n");
    }

    return 0;
}

//==============================================================================
// DLL ENTRY
//==============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        g_bSpecularEnabled = false;
        g_bInitialized = false;
        break;
    }

    return TRUE;
}

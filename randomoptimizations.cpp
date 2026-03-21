//==============================================================================
// GTASAOptimizations.asi - FULLY WORKING VERSION
// Compile: cl /LD /O2 /arch:SSE2 /MT GTASAOptimizations.cpp /link /OUT:GTASAOptimizations.asi d3d9.lib
//==============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "d3d9.lib")

//==============================================================================
// HOOK STATUS
//==============================================================================

typedef enum MH_STATUS {
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED = 1,
    MH_ERROR_NOT_INITIALIZED = 2,
    MH_ERROR_ALREADY_CREATED = 3,
    MH_ERROR_NOT_CREATED = 4,
    MH_ERROR_ENABLED = 5,
    MH_ERROR_DISABLED = 6,
    MH_ERROR_NOT_EXECUTABLE = 7,
    MH_ERROR_UNSUPPORTED_FUNCTION = 8,
    MH_ERROR_MEMORY_ALLOC = 9,
    MH_ERROR_MEMORY_PROTECT = 10,
    MH_ERROR_MODULE_NOT_FOUND = 11,
    MH_ERROR_FUNCTION_NOT_FOUND = 12
} MH_STATUS;

struct SimpleHook {
    void* target;
    void* detour;
    void* original;
    unsigned char savedBytes[16];
    bool enabled;
};

static SimpleHook g_Hooks[32];
static int g_HookCount = 0;
static bool g_MHInitialized = false;

MH_STATUS MH_Initialize() {
    if (g_MHInitialized) return MH_ERROR_ALREADY_INITIALIZED;
    g_MHInitialized = true;
    g_HookCount = 0;
    return MH_OK;
}

MH_STATUS MH_CreateHook(void* pTarget, void* pDetour, void** ppOriginal) {
    if (!g_MHInitialized) return MH_ERROR_NOT_INITIALIZED;
    if (g_HookCount >= 32) return MH_ERROR_MEMORY_ALLOC;

    SimpleHook* hook = &g_Hooks[g_HookCount++];
    hook->target = pTarget;
    hook->detour = pDetour;
    hook->enabled = false;

    memcpy(hook->savedBytes, pTarget, 5);

    if (ppOriginal) {
        *ppOriginal = pTarget;
    }

    return MH_OK;
}

MH_STATUS MH_EnableHook(void* pTarget) {
    for (int i = 0; i < g_HookCount; i++) {
        if (g_Hooks[i].target == pTarget && !g_Hooks[i].enabled) {
            DWORD oldProtect;
            if (!VirtualProtect(pTarget, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                return MH_ERROR_MEMORY_PROTECT;
            }

            *(unsigned char*)pTarget = 0xE9;
            *(int32_t*)((char*)pTarget + 1) = (int32_t)g_Hooks[i].detour - (int32_t)pTarget - 5;

            VirtualProtect(pTarget, 5, oldProtect, &oldProtect);
            g_Hooks[i].enabled = true;
            return MH_OK;
        }
    }
    return MH_ERROR_NOT_CREATED;
}

//==============================================================================
// GLOBALS
//==============================================================================

static IDirect3DDevice9* g_pDevice = nullptr;
static bool g_bInitialized = false;
static bool g_bHooksEnabled = false;
static uint32_t g_FrameCount = 0;

// Stats
static uint32_t g_StateChanges = 0;
static uint32_t g_StatesSaved = 0;

//==============================================================================
// STATE CACHE
//==============================================================================

struct StateCache {
    DWORD renderStates[256];
    IDirect3DTexture9* textures[16];
    bool valid;

    StateCache() : valid(false) {
        Reset();
    }

    void Reset() {
        memset(renderStates, 0xFF, sizeof(renderStates));
        memset(textures, 0, sizeof(textures));
        valid = true;
    }
};

static StateCache g_Cache;

//==============================================================================
// HOOKED FUNCTIONS
//==============================================================================

typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

Present_t g_OrigPresent = nullptr;
SetRenderState_t g_OrigSetRenderState = nullptr;
SetTexture_t g_OrigSetTexture = nullptr;

HRESULT WINAPI Hook_Present(IDirect3DDevice9* pDevice, const RECT* pSourceRect,
    const RECT* pDestRect, HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion) {

    if (!g_pDevice) g_pDevice = pDevice;

    g_FrameCount++;

    if (g_FrameCount % 300 == 0 && g_bHooksEnabled) {
        uint32_t total = g_StateChanges + g_StatesSaved;
        float pct = total > 0 ? (g_StatesSaved * 100.0f / total) : 0.0f;

        char buf[256];
        sprintf(buf, "[Opt] Frame %u: %u states saved / %u total (%.1f%%)\n",
            g_FrameCount, g_StatesSaved, total, pct);
        OutputDebugStringA(buf);

        // Reset stats
        g_StateChanges = 0;
        g_StatesSaved = 0;
    }

    return g_OrigPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT WINAPI Hook_SetRenderState(IDirect3DDevice9* pDevice, D3DRENDERSTATETYPE State, DWORD Value) {
    if (!g_bHooksEnabled || !g_Cache.valid || !pDevice) {
        return g_OrigSetRenderState(pDevice, State, Value);
    }

    if (g_Cache.renderStates[State] == Value) {
        g_StatesSaved++;
        return D3D_OK;
    }

    g_Cache.renderStates[State] = Value;
    g_StateChanges++;
    return g_OrigSetRenderState(pDevice, State, Value);
}

HRESULT WINAPI Hook_SetTexture(IDirect3DDevice9* pDevice, DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    if (!g_bHooksEnabled || !g_Cache.valid || !pDevice || Stage >= 16) {
        return g_OrigSetTexture(pDevice, Stage, pTexture);
    }

    if (g_Cache.textures[Stage] == (IDirect3DTexture9*)pTexture) {
        g_StatesSaved++;
        return D3D_OK;
    }

    g_Cache.textures[Stage] = (IDirect3DTexture9*)pTexture;
    g_StateChanges++;
    return g_OrigSetTexture(pDevice, Stage, pTexture);
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
// SSE2 MEMCPY
//==============================================================================

void* Fast_Memcpy(void* dst, const void* src, size_t size) {
    if (!dst || !src || size == 0) return dst;

    if (size >= 256 &&
        ((uintptr_t)dst & 15) == 0 &&
        ((uintptr_t)src & 15) == 0) {

        __m128i* d = (__m128i*)dst;
        const __m128i* s = (const __m128i*)src;
        size_t count = size / 64;

        for (size_t i = 0; i < count; i++) {
            __m128i v0 = _mm_load_si128(s++);
            __m128i v1 = _mm_load_si128(s++);
            __m128i v2 = _mm_load_si128(s++);
            __m128i v3 = _mm_load_si128(s++);

            _mm_stream_si128(d++, v0);
            _mm_stream_si128(d++, v1);
            _mm_stream_si128(d++, v2);
            _mm_stream_si128(d++, v3);
        }

        _mm_sfence();

        size_t remaining = size & 63;
        if (remaining > 0) {
            memcpy(d, s, remaining);
        }

        return dst;
    }

    return memcpy(dst, src, size);
}

//==============================================================================
// SAFE PATCH
//==============================================================================

void SafePatch(void* addr, const void* data, size_t size) {
    DWORD oldProtect;
    VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(addr, data, size);
    VirtualProtect(addr, size, oldProtect, &oldProtect);
}

void PatchCall(void* addr, void* func) {
    unsigned char call[5] = { 0xE8, 0, 0, 0, 0 };
    *(int32_t*)(call + 1) = (int32_t)func - (int32_t)addr - 5;
    SafePatch(addr, call, 5);
}

//==============================================================================
// INITIALIZATION
//==============================================================================

void InstallHooks() {
    if (g_bHooksEnabled) return;

    OutputDebugStringA("\n========================================\n");
    OutputDebugStringA("  GTA:SA Optimizations v3.0 STABLE\n");
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("[Init] Installing hooks...\n");

    // Get device pointer
    IDirect3DDevice9** ppDevice = (IDirect3DDevice9**)0xC97C28;

    // Wait for valid device
    int timeout = 200;
    while ((!ppDevice || !*ppDevice) && timeout-- > 0) {
        Sleep(100);
    }

    if (!ppDevice || !*ppDevice) {
        OutputDebugStringA("[ERROR] Could not find D3D9 device\n");
        return;
    }

    g_pDevice = *ppDevice;

    // Validate device
    __try {
        UINT refs = g_pDevice->AddRef();
        g_pDevice->Release();

        if (refs == 0) {
            OutputDebugStringA("[ERROR] Device is invalid\n");
            g_pDevice = nullptr;
            return;
        }

        char buf[128];
        sprintf(buf, "[OK] Device: 0x%p (refs: %u)\n", g_pDevice, refs);
        OutputDebugStringA(buf);

    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("[ERROR] Device access violation\n");
        g_pDevice = nullptr;
        return;
    }

    // Initialize cache
    g_Cache.Reset();
    OutputDebugStringA("[OK] State cache initialized\n");

    // Hook via VTable
    int successCount = 0;

    if (HookVTableMethod(g_pDevice, 17, Hook_Present, (void**)&g_OrigPresent)) {
        OutputDebugStringA("[OK] Present hooked\n");
        successCount++;
    }
    else {
        OutputDebugStringA("[FAIL] Present hook failed\n");
    }

    if (HookVTableMethod(g_pDevice, 57, Hook_SetRenderState, (void**)&g_OrigSetRenderState)) {
        OutputDebugStringA("[OK] SetRenderState hooked\n");
        successCount++;
    }
    else {
        OutputDebugStringA("[FAIL] SetRenderState hook failed\n");
    }

    if (HookVTableMethod(g_pDevice, 65, Hook_SetTexture, (void**)&g_OrigSetTexture)) {
        OutputDebugStringA("[OK] SetTexture hooked\n");
        successCount++;
    }
    else {
        OutputDebugStringA("[FAIL] SetTexture hook failed\n");
    }

    // Optional: Hook memcpy (can disable if crashes)
    __try {
        PatchCall((void*)0x4C5869, Fast_Memcpy);
        OutputDebugStringA("[OK] Memcpy optimized\n");
        successCount++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("[SKIP] Memcpy hook skipped\n");
    }

    if (successCount >= 3) {
        g_bHooksEnabled = true;
        g_bInitialized = true;

        char statusBuf[256];
        sprintf(statusBuf, "  %d/%d HOOKS ACTIVE\n", successCount, 4);

        OutputDebugStringA("\n========================================\n");
        OutputDebugStringA(statusBuf);
        OutputDebugStringA("  OPTIMIZATIONS ENABLED\n");
        OutputDebugStringA("========================================\n\n");
    }
    else {
        OutputDebugStringA("\n[ERROR] Too few hooks succeeded\n");
    }
}

//==============================================================================
// INIT THREAD
//==============================================================================

DWORD WINAPI InitThread(LPVOID) {
    OutputDebugStringA("[Init] Waiting 15 seconds for game...\n");
    Sleep(15000);

    __try {
        InstallHooks();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("[CRITICAL] Hook installation crashed!\n");
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
        g_bHooksEnabled = false;
        g_bInitialized = false;
        break;
    }

    return TRUE;
}

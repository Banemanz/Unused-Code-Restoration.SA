// Restores the leak-era PC memory-manager semaphore idea as a single-file
// Plugin-SDK example for GTA SA 1.0 US.
//
// Leak context:
// - `Partial SRC Leak/MemoryMgr.cpp` contains real PC `CreateSemaphore` setup for
//   `memMgrSema` / `scratchPadSema`, but the actual `WaitForSingleObject` and
//   `ReleaseSemaphore` calls inside `LockCriticalCode` / `ReleaseCriticalCode`
//   were commented out on PC.
// - This file resurrects that intent in a practical way by wrapping the shipped
//   `CMemoryMgr` allocator entry points with a real process-local semaphore.
//
// Scope of this example:
// - Hooks the retail PC `CMemoryMgr::Malloc`, `Free`, and `Realloc` entry points at runtime.
// - Uses leak-shaped semaphore semantics, but adds same-thread recursion tracking
//   so normal re-entrant allocator paths do not deadlock.
// - Avoids extra SDK-local headers so it stays portable in a normal generated
//   Plugin-SDK project.
// - The in-file x86 trampoline hook is left disabled by default because its
//   prologue assumptions are not yet verified against a live retail binary.

#include "plugin.h"
#include "common.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

using namespace plugin;

namespace {
    using uint32 = std::uint32_t;
    using uint8 = std::uint8_t;

    constexpr uintptr_t ADDR_MEMORYMGR_MALLOC = 0x72F420;
    constexpr uintptr_t ADDR_MEMORYMGR_FREE = 0x72F430;
    constexpr uintptr_t ADDR_MEMORYMGR_REALLOC = 0x72F440;
    constexpr size_t    JMP_PATCH_SIZE = 5;
    constexpr bool      ENABLE_UNVERIFIED_ALLOCATOR_HOOKS = false;

    struct LeakStyleSemaphore {
        HANDLE   handle{};
        DWORD    ownerThreadId{};
        uint32   recursion{};

        void Init(long initialCount = 1, long maxCount = 1) {
            handle = CreateSemaphoreA(nullptr, initialCount, maxCount, nullptr);
        }

        void Lock() {
            const DWORD currentThreadId = GetCurrentThreadId();
            if (ownerThreadId == currentThreadId) {
                ++recursion;
                return;
            }

            WaitForSingleObject(handle, INFINITE);
            ownerThreadId = currentThreadId;
            recursion = 1;
        }

        void Unlock() {
            const DWORD currentThreadId = GetCurrentThreadId();
            if (ownerThreadId != currentThreadId || recursion == 0) {
                return;
            }

            if (--recursion == 0) {
                ownerThreadId = 0;
                ReleaseSemaphore(handle, 1, nullptr);
            }
        }
    };

    class ScopedLeakSemaphore {
    public:
        explicit ScopedLeakSemaphore(LeakStyleSemaphore& semaphore) : m_semaphore(semaphore) {
            m_semaphore.Lock();
        }

        ~ScopedLeakSemaphore() {
            m_semaphore.Unlock();
        }

    private:
        LeakStyleSemaphore& m_semaphore;
    };

    struct X86InlineHook {
        void* target{};
        void* trampoline{};
        uint8 originalBytes[JMP_PATCH_SIZE]{};

        template <typename T>
        T Original() const {
            return reinterpret_cast<T>(trampoline);
        }
    };

    static void WriteJump(void* src, const void* dst) {
        DWORD oldProtect{};
        VirtualProtect(src, JMP_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);

        auto* bytes = static_cast<uint8*>(src);
        bytes[0] = 0xE9;
        *reinterpret_cast<std::int32_t*>(bytes + 1) =
            static_cast<std::int32_t>(reinterpret_cast<uintptr_t>(dst) - reinterpret_cast<uintptr_t>(src) - JMP_PATCH_SIZE);

        VirtualProtect(src, JMP_PATCH_SIZE, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), src, JMP_PATCH_SIZE);
    }

    static bool InstallInlineHook(X86InlineHook& hook, void* target, void* detour) {
        hook.target = target;
        std::memcpy(hook.originalBytes, target, JMP_PATCH_SIZE);

        hook.trampoline = VirtualAlloc(nullptr, JMP_PATCH_SIZE + JMP_PATCH_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hook.trampoline) {
            return false;
        }

        std::memcpy(hook.trampoline, hook.originalBytes, JMP_PATCH_SIZE);
        WriteJump(static_cast<uint8*>(hook.trampoline) + JMP_PATCH_SIZE, static_cast<uint8*>(target) + JMP_PATCH_SIZE);
        WriteJump(target, detour);
        return true;
    }

    class RestoredMemoryMgrSemaphoresSingleCpp;
    static RestoredMemoryMgrSemaphoresSingleCpp* g_memoryMgrSemaphores = nullptr;

    class RestoredMemoryMgrSemaphoresSingleCpp {
    public:
        RestoredMemoryMgrSemaphoresSingleCpp() {
            g_memoryMgrSemaphores = this;
            m_memMgrSema.Init();

            Events::initGameEvent += [] {
                if (g_memoryMgrSemaphores && ENABLE_UNVERIFIED_ALLOCATOR_HOOKS) {
                    g_memoryMgrSemaphores->InstallHooksOnce();
                }
                };
        }

    private:
        using MallocFn = void* (__cdecl*)(uint32 size);
        using FreeFn = void(__cdecl*)(void* memory);
        using ReallocFn = uint8 * (__cdecl*)(void* memory, uint32 size);

        bool               m_installed{};
        LeakStyleSemaphore m_memMgrSema{};
        X86InlineHook      m_mallocHook{};
        X86InlineHook      m_freeHook{};
        X86InlineHook      m_reallocHook{};

        void InstallHooksOnce() {
            if (m_installed) {
                return;
            }

            const bool mallocInstalled = InstallInlineHook(m_mallocHook, reinterpret_cast<void*>(ADDR_MEMORYMGR_MALLOC), reinterpret_cast<void*>(HookedMalloc));
            const bool freeInstalled = InstallInlineHook(m_freeHook, reinterpret_cast<void*>(ADDR_MEMORYMGR_FREE), reinterpret_cast<void*>(HookedFree));
            const bool reallocInstalled = InstallInlineHook(m_reallocHook, reinterpret_cast<void*>(ADDR_MEMORYMGR_REALLOC), reinterpret_cast<void*>(HookedRealloc));

            m_installed = mallocInstalled && freeInstalled && reallocInstalled;
        }

        static RestoredMemoryMgrSemaphoresSingleCpp& Instance() {
            return *g_memoryMgrSemaphores;
        }

        static void* __cdecl HookedMalloc(uint32 size) {
            ScopedLeakSemaphore lock(Instance().m_memMgrSema);
            return Instance().m_mallocHook.Original<MallocFn>()(size);
        }

        static void __cdecl HookedFree(void* memory) {
            ScopedLeakSemaphore lock(Instance().m_memMgrSema);
            Instance().m_freeHook.Original<FreeFn>()(memory);
        }

        static uint8* __cdecl HookedRealloc(void* memory, uint32 size) {
            ScopedLeakSemaphore lock(Instance().m_memMgrSema);
            return Instance().m_reallocHook.Original<ReallocFn>()(memory, size);
        }
    };

    static RestoredMemoryMgrSemaphoresSingleCpp gRestoredMemoryMgrSemaphoresSingleCpp;
}

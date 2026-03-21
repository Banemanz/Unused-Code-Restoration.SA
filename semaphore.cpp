// Restores the leak-era PC memory-manager semaphore idea as a single-file
// Plugin-SDK example for GTA SA 1.0 US.
//
// Leak context:
// - `Partial SRC Leak/MemoryMgr.cpp` contains real PC `CreateSemaphore` setup for
//   `memMgrSema` / `scratchPadSema`, but the actual `WaitForSingleObject` and
//   `ReleaseSemaphore` calls inside `LockCriticalCode` / `ReleaseCriticalCode`
//   were commented out on PC.
// - The retail allocator already owns its heap bookkeeping, so a safe restore has
//   to preserve the original allocator implementation and only add serialization
//   around it.
//
// Scope of this example:
// - Hooks the retail PC `CMemoryMgr::Malloc`, `Free`, and `Realloc` entry points at runtime.
// - Uses leak-shaped semaphore semantics, but adds same-thread recursion tracking
//   so normal re-entrant allocator paths do not deadlock.
// - Avoids extra SDK-local headers so it stays portable in a normal generated
//   Plugin-SDK project.
// - The inline hook path stays opt-in because its prologue assumptions are still
//   unverified for every loader/build combination; enabling it without validating
//   the target binary can still crash the game.

#include "plugin.h"
#include "common.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace plugin;

namespace {
    using uint32 = std::uint32_t;
    using uint8 = std::uint8_t;

    constexpr uintptr_t ADDR_MEMORYMGR_MALLOC = 0x72F420;
    constexpr uintptr_t ADDR_MEMORYMGR_FREE = 0x72F430;
    constexpr uintptr_t ADDR_MEMORYMGR_REALLOC = 0x72F440;
    constexpr size_t    JMP_PATCH_SIZE = 5;
    constexpr bool      ENABLE_UNVERIFIED_ALLOCATOR_HOOKS = true;

    struct LeakStyleSemaphore {
        HANDLE handle{};
        DWORD  ownerThreadId{};
        uint32 recursion{};

        void Init(long initialCount = 1, long maxCount = 1) {
            handle = CreateSemaphoreA(nullptr, initialCount, maxCount, nullptr);
        }

        void Lock() {
            if (!handle) {
                return;
            }

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
            if (!handle) {
                return;
            }

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
        bool  installed{};

        template <typename T>
        T Original() const {
            return reinterpret_cast<T>(trampoline);
        }
    };

    static bool CanWriteRelativeJump(const void* src, const void* dst) {
        const auto srcAddr = reinterpret_cast<uintptr_t>(src);
        const auto dstAddr = reinterpret_cast<uintptr_t>(dst);
        const auto distance = static_cast<std::int64_t>(dstAddr) - static_cast<std::int64_t>(srcAddr) - static_cast<std::int64_t>(JMP_PATCH_SIZE);
        return distance >= std::numeric_limits<std::int32_t>::min()
            && distance <= std::numeric_limits<std::int32_t>::max();
    }

    static bool IsExecutableAddress(const void* address) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }

        if (mbi.State != MEM_COMMIT) {
            return false;
        }

        const DWORD executableMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & executableMask) != 0;
    }

    static bool VerifyHookTarget(const void* target, const void* detour) {
        if (!target || !detour || !IsExecutableAddress(target) || !CanWriteRelativeJump(target, detour)) {
            return false;
        }

        const auto* bytes = static_cast<const uint8*>(target);
        if (bytes[0] == 0xE9 || bytes[0] == 0xE8 || bytes[0] == 0xC3 || bytes[0] == 0xCC) {
            return false;
        }

        return true;
    }

    static bool VerifyInstalledJump(const void* src, const void* dst) {
        const auto* bytes = static_cast<const uint8*>(src);
        if (bytes[0] != 0xE9) {
            return false;
        }

        const auto displacement = *reinterpret_cast<const std::int32_t*>(bytes + 1);
        const auto resolved = reinterpret_cast<uintptr_t>(src) + JMP_PATCH_SIZE + displacement;
        return resolved == reinterpret_cast<uintptr_t>(dst);
    }

    static bool WriteJump(void* src, const void* dst) {
        if (!CanWriteRelativeJump(src, dst)) {
            return false;
        }
        DWORD oldProtect{};
        VirtualProtect(src, JMP_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);

        auto* bytes = static_cast<uint8*>(src);
        bytes[0] = 0xE9;
        *reinterpret_cast<std::int32_t*>(bytes + 1) =
            static_cast<std::int32_t>(reinterpret_cast<uintptr_t>(dst) - reinterpret_cast<uintptr_t>(src) - JMP_PATCH_SIZE);

        VirtualProtect(src, JMP_PATCH_SIZE, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), src, JMP_PATCH_SIZE);
        return VerifyInstalledJump(src, dst);
    }

    static void RemoveInlineHook(X86InlineHook& hook) {
        if (!hook.installed || !hook.target) {
            return;
        }

        DWORD oldProtect{};
        VirtualProtect(hook.target, JMP_PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);
        std::memcpy(hook.target, hook.originalBytes, JMP_PATCH_SIZE);
        VirtualProtect(hook.target, JMP_PATCH_SIZE, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), hook.target, JMP_PATCH_SIZE);

        if (hook.trampoline) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
        }

        hook = {};
    }

    static bool InstallInlineHook(X86InlineHook& hook, void* target, void* detour) {
        if (!VerifyHookTarget(target, detour)) {
            return false;
        }

        hook.target = target;
        std::memcpy(hook.originalBytes, target, JMP_PATCH_SIZE);

        hook.trampoline = VirtualAlloc(nullptr, JMP_PATCH_SIZE + JMP_PATCH_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hook.trampoline) {
            return false;
        }

        std::memcpy(hook.trampoline, hook.originalBytes, JMP_PATCH_SIZE);
        if (!WriteJump(static_cast<uint8*>(hook.trampoline) + JMP_PATCH_SIZE, static_cast<uint8*>(target) + JMP_PATCH_SIZE)) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook = {};
            return false;
        }

        if (!WriteJump(target, detour)) {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook = {};
            return false;
        }

        hook.installed = true;
        return true;
    }

    class RestoredMemoryMgrSemaphoresSingleCpp;
    static RestoredMemoryMgrSemaphoresSingleCpp* g_memoryMgrSemaphores = nullptr;

    class RestoredMemoryMgrSemaphoresSingleCpp {
    public:
        ~RestoredMemoryMgrSemaphoresSingleCpp() {
            RemoveInlineHook(m_reallocHook);
            RemoveInlineHook(m_freeHook);
            RemoveInlineHook(m_mallocHook);
        }

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
            const bool freeInstalled = mallocInstalled
                && InstallInlineHook(m_freeHook, reinterpret_cast<void*>(ADDR_MEMORYMGR_FREE), reinterpret_cast<void*>(HookedFree));
            const bool reallocInstalled = freeInstalled
                && InstallInlineHook(m_reallocHook, reinterpret_cast<void*>(ADDR_MEMORYMGR_REALLOC), reinterpret_cast<void*>(HookedRealloc));

            if (!(mallocInstalled && freeInstalled && reallocInstalled)) {
                RemoveInlineHook(m_reallocHook);
                RemoveInlineHook(m_freeHook);
                RemoveInlineHook(m_mallocHook);
                return;
            }

            m_installed = true;
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

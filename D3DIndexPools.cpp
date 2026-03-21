// Exposes and manages GTA SA's shipped D3D resource pooling system as a
// single-file Plugin-SDK example for GTA SA 1.0 US.
//
// Important note:
// - The preserved leak contains an older `TexturePool.cpp` / `IndexBufferPool`
//   implementation and helper names such as `D3DPoolsInit`.
// - San Andreas PC did not simply lose this functionality; the retail game still
//   ships a pooled Direct3D resource system, exposed in Plugin-SDK as
//   `D3DResourceSystem`, `D3DTextureBuffer`, and `D3DIndexDataBuffer`.
// - So this file intentionally does not recreate duplicate global arrays from the
//   leak. Instead, it restores an equivalent control/debug surface around the
//   real in-game subsystem.
// - `CancelBuffering()` is intentionally *not* exposed as a gameplay hotkey here.
//   The retail game uses it during RenderWare shutdown, and forcing it manually in
//   arbitrary runtime states can dereference invalid pooled resources.
//
// Controls:
// - F7: Toggle D3D resource buffering on/off.
// - F8: Show current buffering state, occupancy, capacities, and memory usage.
// - F9: Run a conservative tidy pass on pooled textures/index buffers.
// - F10: Cycle the pool-capacity profile (stock / 2x / 4x).
//
// Default behavior:
// - Buffering is enabled automatically after RenderWare initialization.
// - Pool capacities are upscaled once at RW init, with a conservative 4x profile by
//   default to better suit modern PCs without rewriting the engine subsystem.

#include "plugin.h"
#include "common.h"
#include "CMessages.h"
#include "CTimer.h"
#include "D3DResourceSystem.h"

#include <algorithm>
#include <cstdio>

using namespace plugin;

namespace {
    constexpr unsigned TOGGLE_COOLDOWN_MS = 250;
    constexpr int      MESSAGE_TIME_MS = 1800;
    constexpr unsigned TIDY_TEXTURE_COUNT = 2;
    constexpr unsigned TIDY_INDEX_COUNT = 4;
    constexpr unsigned DEFAULT_PROFILE_INDEX = 2; // stock / 2x / 4x

    class RestoredD3DResourcePoolsSingleCpp;
    static RestoredD3DResourcePoolsSingleCpp* g_restoredD3DPools = nullptr;

    class RestoredD3DResourcePoolsSingleCpp {
    public:
        RestoredD3DResourcePoolsSingleCpp() {
            g_restoredD3DPools = this;

            Events::initRwEvent += [] {
                if (g_restoredD3DPools) {
                    g_restoredD3DPools->OnRwInit();
                }
                };

            Events::shutdownRwEvent += [] {
                if (g_restoredD3DPools) {
                    g_restoredD3DPools->m_rwReady = false;
                }
                };

            Events::gameProcessEvent += [] {
                if (g_restoredD3DPools) {
                    g_restoredD3DPools->Process();
                }
                };
        }

    private:
        enum class PoolProfile : unsigned {
            Stock = 0,
            Double = 1,
            Quad = 2
        };

        bool        m_rwReady{};
        bool        m_defaultsApplied{};
        unsigned    m_nextInputTime{};
        unsigned    m_baseTextureCapacity{};
        unsigned    m_baseIndexCapacity{};
        PoolProfile m_profile{ static_cast<PoolProfile>(DEFAULT_PROFILE_INDEX) };

        void OnRwInit() {
            m_rwReady = true;
            m_defaultsApplied = false;
            m_baseTextureCapacity = GetTextureCapacity();
            m_baseIndexCapacity = GetIndexCapacity();
            ApplyDefaultPolicy();
        }

        void ApplyDefaultPolicy() {
            if (!m_rwReady || m_defaultsApplied) {
                return;
            }

            D3DResourceSystem::SetUseD3DResourceBuffering(true);
            ApplyPoolProfile(m_profile);
            m_defaultsApplied = true;
        }

        void Process() {
            ApplyDefaultPolicy();

            if (!m_rwReady || CTimer::m_snTimeInMilliseconds < m_nextInputTime) {
                return;
            }

            if (KeyPressed(VK_F7)) {
                m_nextInputTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
                ToggleBuffering();
                return;
            }

            if (KeyPressed(VK_F8)) {
                m_nextInputTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
                ShowStats();
                return;
            }

            if (KeyPressed(VK_F9)) {
                m_nextInputTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
                TidyPools();
                return;
            }

            if (KeyPressed(VK_F10)) {
                m_nextInputTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
                CyclePoolProfile();
                return;
            }
        }

        static unsigned GetPixelBytes() {
            return D3DResourceSystem::GetTotalPixelsSize();
        }

        static unsigned GetIndexBytes() {
            return D3DResourceSystem::GetTotalIndexDataSize();
        }

        static unsigned GetTextureCapacity() {
            return D3DResourceSystem::TextureBuffer.m_nCapcacity;
        }

        static unsigned GetTextureCount() {
            return D3DResourceSystem::TextureBuffer.m_nNumTexturesInBuffer;
        }

        static unsigned GetIndexCapacity() {
            return D3DResourceSystem::IndexDataBuffer.m_nCapcacity;
        }

        static unsigned GetIndexCount() {
            return D3DResourceSystem::IndexDataBuffer.m_nNumDatasInBuffer;
        }

        static unsigned GetProfileMultiplier(PoolProfile profile) {
            switch (profile) {
            case PoolProfile::Stock:  return 1;
            case PoolProfile::Double: return 2;
            case PoolProfile::Quad:   return 4;
            default:                  return 1;
            }
        }

        static const char* GetProfileName(PoolProfile profile) {
            switch (profile) {
            case PoolProfile::Stock:  return "stock";
            case PoolProfile::Double: return "2x";
            case PoolProfile::Quad:   return "4x";
            default:                  return "unknown";
            }
        }

        void ToggleBuffering() {
            const bool newState = !D3DResourceSystem::UseD3DResourceBuffering;
            D3DResourceSystem::SetUseD3DResourceBuffering(newState);
            ShowStats(newState ? "D3D resource buffering on" : "D3D resource buffering off");
        }

        void CyclePoolProfile() {
            const unsigned next = (static_cast<unsigned>(m_profile) + 1u) % 3u;
            m_profile = static_cast<PoolProfile>(next);
            ApplyPoolProfile(m_profile);
            ShowStats("D3D pool profile changed");
        }

        static void ApplyPoolProfile(PoolProfile profile) {
            const unsigned multiplier = GetProfileMultiplier(profile);
            const unsigned textureCapacity = std::max(16u, g_restoredD3DPools ? g_restoredD3DPools->m_baseTextureCapacity * multiplier : GetTextureCapacity() * multiplier);
            const unsigned indexCapacity = std::max(32u, g_restoredD3DPools ? g_restoredD3DPools->m_baseIndexCapacity * multiplier : GetIndexCapacity() * multiplier);

            if (textureCapacity > GetTextureCapacity()) {
                D3DResourceSystem::TextureBuffer.Resize(textureCapacity);
            }

            if (indexCapacity > GetIndexCapacity()) {
                D3DResourceSystem::IndexDataBuffer.Resize(indexCapacity);
            }
        }

        void TidyPools() {
            if (!D3DResourceSystem::UseD3DResourceBuffering) {
                CMessages::AddMessageJumpQ("D3D buffering is disabled", MESSAGE_TIME_MS, 0, false);
                return;
            }

            if (GetPixelBytes() == 0 && GetIndexBytes() == 0) {
                CMessages::AddMessageJumpQ("D3D pools are already empty", MESSAGE_TIME_MS, 0, false);
                return;
            }

            D3DResourceSystem::TidyUpD3DTextures(TIDY_TEXTURE_COUNT);
            D3DResourceSystem::TidyUpD3DIndexBuffers(TIDY_INDEX_COUNT);
            ShowStats("D3D resource pools tidied");
        }

        void ShowStats(const char* prefix = nullptr) const {
            const unsigned pixelBytes = GetPixelBytes();
            const unsigned indexBytes = GetIndexBytes();
            const unsigned totalBytes = pixelBytes + indexBytes;

            char msg[160]{};
            std::snprintf(
                msg,
                sizeof(msg),
                "%s%s%s %s | Tex %u/%u | Idx %u/%u | %u KB",
                prefix ? prefix : "D3D pools",
                prefix ? " |" : ":",
                D3DResourceSystem::UseD3DResourceBuffering ? " on" : " off",
                GetProfileName(m_profile),
                GetTextureCount(),
                GetTextureCapacity(),
                GetIndexCount(),
                GetIndexCapacity(),
                totalBytes / 1024
            );
            CMessages::AddMessageJumpQ(msg, MESSAGE_TIME_MS, 0, false);
        }
    };

    static RestoredD3DResourcePoolsSingleCpp gRestoredD3DResourcePoolsSingleCpp;
}

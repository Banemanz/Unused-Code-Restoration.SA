// Shadow-map / real-time-shadow single-file example for GTA SA 1.0 US / Plugin-SDK.
//
// Leak context:
// - `Partial SRC Leak/Main.cpp` includes `VolumetricShadowMgr.h` and calls
//   `CVolumetricShadowMgr::Render()` in the main render path.
// - `Partial SRC Leak/gtafiles.txt` also proves `Renderer/VolumetricShadowMgr.*`
//   existed in Rockstar's SA PC tree.
// - The actual volumetric manager implementation is still missing here, so this
//   file takes a more honest route than the earlier blob-shadow stack prototype:
//   it drives the retail real-time shadow camera path instead.
//
// Practical restore strategy:
// - use the game's existing real-time shadow manager (`0xC40350`) to request
//   shadow-map updates for the player, nearby peds, and nearby vehicles,
// - keep the file self-contained so it can drop into a generated Plugin-SDK
//   project without depending on newer reverse-only headers.
//
// Controls:
// - F7: Toggle the effect on/off.

#include "plugin.h"
#include "common.h"
#include "CMessages.h"
#include "CPad.h"
#include "CPed.h"
#include "CPools.h"
#include "CTimer.h"
#include "CVehicle.h"

#include <cstdint>

using namespace plugin;

namespace {
    using uint8 = std::uint8_t;

    constexpr unsigned TOGGLE_COOLDOWN_MS = 300;
    constexpr int      MESSAGE_TIME_MS = 1500;
    constexpr float    PED_RENDER_RADIUS = 35.0f;
    constexpr float    VEH_RENDER_RADIUS = 55.0f;
    constexpr uintptr_t ADDR_REALTIME_SHADOW_MANAGER = 0xC40350;
    constexpr uintptr_t ADDR_DO_SHADOW_THIS_FRAME = 0x706BA0;

    struct CRealTimeShadowManagerStub {
        uint8 m_bInitialised{};
        uint8 m_bNeedsReinit{};
    };

    static CRealTimeShadowManagerStub& g_realTimeShadowMan = *reinterpret_cast<CRealTimeShadowManagerStub*>(ADDR_REALTIME_SHADOW_MANAGER);

    static float DistanceToPlayer(const CEntity* entity, const CPed* player) {
        if (!entity || !player) {
            return 99999.0f;
        }
        return DistanceBetweenPoints(entity->GetPosition(), player->GetPosition());
    }

    static void RequestRealTimeShadow(CPhysical* physical) {
        if (!physical) {
            return;
        }

        plugin::CallMethod<ADDR_DO_SHADOW_THIS_FRAME, CRealTimeShadowManagerStub*, CPhysical*>(&g_realTimeShadowMan, physical);
    }

    class RestoredVolumetricShadowsSingleCpp {
    public:
        RestoredVolumetricShadowsSingleCpp() {
            ms_instance = this;
            Events::gameProcessEvent += [] {
                if (ms_instance) {
                    ms_instance->Process();
                }
                };
        }

    private:
        static RestoredVolumetricShadowsSingleCpp* ms_instance;

        bool     m_enabled{ true };
        unsigned m_nextToggleTime{};

        void Process() {
            ProcessToggle();
            if (!m_enabled) {
                return;
            }

            if (!g_realTimeShadowMan.m_bInitialised) {
                return;
            }

            auto* player = FindPlayerPed();
            if (!player || !player->IsAlive() || !player->m_pRwObject) {
                return;
            }

            RequestRealTimeShadow(player);
            RenderNearbyPeds(player);
            RenderNearbyVehicles(player);
        }

        void ProcessToggle() {
            if (CTimer::m_snTimeInMilliseconds < m_nextToggleTime) {
                return;
            }

            if (!KeyPressed(VK_F7)) {
                return;
            }

            m_nextToggleTime = CTimer::m_snTimeInMilliseconds + TOGGLE_COOLDOWN_MS;
            m_enabled = !m_enabled;
            CMessages::AddMessageJumpQ(m_enabled ? "Shadow-map restore on" : "Shadow-map restore off", MESSAGE_TIME_MS, 0, false);
        }

        void RenderNearbyPeds(CPed* player) {
            if (!CPools::ms_pPedPool) {
                return;
            }

            for (int i = 0; i < CPools::ms_pPedPool->m_nSize; ++i) {
                auto* ped = CPools::ms_pPedPool->GetAt(i);
                if (!ped || ped == player || !ped->IsAlive() || !ped->m_pRwObject || ped->bInVehicle || ped->m_pVehicle || ped->bDontRender) {
                    continue;
                }

                if (DistanceToPlayer(ped, player) > PED_RENDER_RADIUS) {
                    continue;
                }

                RequestRealTimeShadow(ped);
            }
        }

        void RenderNearbyVehicles(CPed* player) {
            if (!CPools::ms_pVehiclePool) {
                return;
            }

            for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; ++i) {
                auto* vehicle = CPools::ms_pVehiclePool->GetAt(i);
                if (!vehicle || !vehicle->m_pRwObject) {
                    continue;
                }

                if (DistanceToPlayer(vehicle, player) > VEH_RENDER_RADIUS) {
                    continue;
                }

                RequestRealTimeShadow(vehicle);
            }
        }
    };

    RestoredVolumetricShadowsSingleCpp* RestoredVolumetricShadowsSingleCpp::ms_instance = nullptr;
    static RestoredVolumetricShadowsSingleCpp gRestoredVolumetricShadowsSingleCpp;
}
